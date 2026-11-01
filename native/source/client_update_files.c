#include "client_update_internal.h"
#include "diagnostics.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <switch.h>

#define MANIFEST UPDATE_WORK "/manifest"
#define INSTALLING UPDATE_WORK "/installing"
#define COMPLETE UPDATE_WORK "/complete"
typedef struct {
    char magic[8];
    uint32_t count, checksum;
} ManifestHeader;
static const char manifest_magic[8] = {'P', 'K', 'M', 'N', 'U', 'P', '1', 0};

bool updateCommit(void) {
    Result rc = fsdevCommitDevice("sdmc");
    if (R_FAILED(rc)) diagnosticsTrace("update.commit=ERROR rc=0x%x", rc);
    return R_SUCCEEDED(rc);
}
bool updateMakeParents(const char *path) {
    char copy[512];
    if (strlen(path) >= sizeof(copy)) return false;
    strcpy(copy, path);
    char *start = strchr(copy, ':');
    start = start ? start + 1 : copy;
    for (char *at = start + 1; *at; ++at) {
        if (*at != '/') continue;
        *at = 0;
        struct stat info;
        bool ok = (!mkdir(copy, 0700) || errno == EEXIST) && !stat(copy, &info) && S_ISDIR(info.st_mode);
        *at = '/';
        if (!ok) return false;
    }
    return true;
}
static bool removeFile(const char *path) { return !unlink(path) || errno == ENOENT; }
// Only called for the updater's fixed private directory. Never follows links or enters the game directory.
static bool cleanFolder(const char *folder, unsigned depth) {
    if (depth > 32) return false;
    DIR *directory = opendir(folder);
    if (!directory) return errno == ENOENT;
    bool ok = true;
    for (struct dirent *entry; ok && (entry = readdir(directory)) != NULL;) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char path[768];
        if (snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name) >= (int)sizeof(path)) {
            ok = false;
            break;
        }
        struct stat info;
        if (lstat(path, &info)) {
            ok = false;
            break;
        }
        ok = S_ISDIR(info.st_mode) ? cleanFolder(path, depth + 1) : removeFile(path);
    }
    closedir(directory);
    return ok && (!rmdir(folder) || errno == ENOENT);
}
bool updateClean(void) { return cleanFolder(UPDATE_WORK, 0); }
static bool finishFile(FILE *file) {
    bool ok = fflush(file) == 0;
    if (ok && fsync(fileno(file))) ok = false;
    if (fclose(file)) ok = false;
    return ok;
}
static bool writeMarker(const char *path, const char value[4]) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fwrite(value, 1, 4, file) == 4;
    return finishFile(file) && ok && updateCommit();
}
static bool completeMarker(void) {
    FILE *file = fopen(COMPLETE, "rb");
    if (!file) return false;
    char value[5];
    size_t bytes = fread(value, 1, sizeof(value), file);
    bool ok = !ferror(file) && bytes == 4 && !memcmp(value, "done", 4);
    fclose(file);
    return ok;
}
static bool validEntries(const UpdateEntry *entries, unsigned count) {
    if (!count || count > UPDATE_MAX_FILES) return false;
    bool client = false;
    uint64_t total = 0;
    for (unsigned i = 0; i < count; ++i) {
        const UpdateEntry *entry = &entries[i];
        if (!memchr(entry->name, 0, sizeof(entry->name)) || !updateManagedName(entry->name) || entry->had_old > 1 || entry->bytes > UPDATE_MAX_EXPANDED)
            return false;
        total += entry->bytes;
        if (total > UPDATE_MAX_EXPANDED) return false;
        for (unsigned j = 0; j < i; ++j)
            if (!strcasecmp(entries[j].name, entry->name)) return false;
        if (!strcmp(entry->name, "bin/linux/arm64/PokeMMO")) client = true;
    }
    return client && !strcmp(entries[count - 1].name, "revision.txt");
}
static bool saveManifest(const UpdateEntry *entries, unsigned count) {
    ManifestHeader header = {.count = count, .checksum = (uint32_t)crc32(0, (const Bytef *)entries, count * sizeof(*entries))};
    memcpy(header.magic, manifest_magic, sizeof(header.magic));
    FILE *file = fopen(MANIFEST ".part", "wb");
    if (!file) return false;
    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header) && fwrite(entries, sizeof(*entries), count, file) == count;
    return finishFile(file) && ok && !rename(MANIFEST ".part", MANIFEST) && updateCommit();
}
static UpdateEntry *loadManifest(unsigned *count) {
    *count = 0;
    FILE *file = fopen(MANIFEST, "rb");
    if (!file) return NULL;
    ManifestHeader header;
    UpdateEntry *entries = NULL;
    if (fread(&header, 1, sizeof(header), file) != sizeof(header) || memcmp(header.magic, manifest_magic, sizeof(header.magic)) || !header.count ||
        header.count > UPDATE_MAX_FILES)
        goto done;
    entries = calloc(header.count, sizeof(*entries));
    if (!entries) goto done;
    bool ok = fread(entries, sizeof(*entries), header.count, file) == header.count && fgetc(file) == EOF && !ferror(file) &&
              (uint32_t)crc32(0, (const Bytef *)entries, header.count * sizeof(*entries)) == header.checksum && validEntries(entries, header.count);
    if (!ok) {
        free(entries);
        entries = NULL;
    } else
        *count = header.count;
done:
    fclose(file);
    return entries;
}
bool updateInstall(UpdateJob *job, const UpdateEntry *entries, unsigned count) {
    if (!validEntries(entries, count) || updateCancelled(job) || !saveManifest(entries, count) || !writeMarker(INSTALLING, "swap")) return false;
    updateProgress(job, UPDATE_INSTALL, 950);
    for (unsigned i = 0; i < count; ++i) {
        char source[512], target[512], backup[512];
        snprintf(source, sizeof(source), UPDATE_WORK "/stage/%s", entries[i].name);
        snprintf(target, sizeof(target), UPDATE_GAME "/%s", entries[i].name);
        snprintf(backup, sizeof(backup), UPDATE_WORK "/backup/%s", entries[i].name);
        if (updateCancelled(job) || !updateMakeParents(target) || !updateMakeParents(backup)) return false;
        struct stat info;
        if (entries[i].had_old) {
            if (stat(target, &info) || !S_ISREG(info.st_mode) || !lstat(backup, &info) || errno != ENOENT || rename(target, backup) || !updateCommit())
                return false;
        } else if (!lstat(target, &info) || errno != ENOENT)
            return false;
        if (rename(source, target) || !updateCommit()) return false;
        updateProgress(job, UPDATE_INSTALL, 950 + (i + 1) * 50 / count);
    }
    if (!writeMarker(COMPLETE, "done")) return false;
    // The new installation is committed. A leftover complete marker only requires housekeeping on the next launch.
    if (removeFile(INSTALLING) && updateCommit()) updateClean();
    return true;
}
bool updateRecover(UpdateJob *job) {
    struct stat marker;
    if (stat(INSTALLING, &marker)) {
        if (errno != ENOENT) return false;
        if (completeMarker() && updateCommit()) updateClean();
        return true;
    }
    if (completeMarker()) {
        if (removeFile(INSTALLING) && updateCommit()) updateClean();
        return true;
    }
    unsigned count;
    UpdateEntry *entries = loadManifest(&count);
    if (!entries) return false;
    updateProgress(job, UPDATE_INSTALL, 0);
    bool ok = true;
    for (unsigned i = count; ok && i > 0; --i) {
        const UpdateEntry *entry = &entries[i - 1];
        char target[512], source[512], backup[512];
        snprintf(target, sizeof(target), UPDATE_GAME "/%s", entry->name);
        snprintf(source, sizeof(source), UPDATE_WORK "/stage/%s", entry->name);
        snprintf(backup, sizeof(backup), UPDATE_WORK "/backup/%s", entry->name);
        struct stat info;
        if (!lstat(backup, &info)) {
            ok = S_ISREG(info.st_mode) && entry->had_old && removeFile(target) && updateMakeParents(target) && !rename(backup, target) && updateCommit();
        } else if (errno != ENOENT)
            ok = false;
        else if (entry->had_old)
            ok = !stat(target, &info) && S_ISREG(info.st_mode);
        else if (stat(source, &info)) {
            if (errno == ENOENT)
                ok = removeFile(target) && updateCommit();
            else
                ok = false;
        }
        updateProgress(job, UPDATE_INSTALL, (count - i + 1) * 1000 / count);
    }
    free(entries);
    if (!ok || !removeFile(INSTALLING) || !updateCommit()) return false;
    updateClean();
    diagnosticsTrace("update.rollback=COMPLETE");
    return true;
}
