#include "client_update_internal.h"
#include "elf_layout.h"
#include <errno.h>
#include <minizip/unzip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static uint16_t word(const unsigned char *p) { return (uint16_t)(p[0] | ((unsigned)p[1] << 8)); }
static uint32_t dword(const unsigned char *p) { return (uint32_t)word(p) | ((uint32_t)word(p + 2) << 16); }
static uint32_t revisionText(const unsigned char *text, size_t bytes) {
    uint64_t value = 0;
    size_t used = 0;
    while (used < bytes && text[used] >= '0' && text[used] <= '9') {
        value = value * 10 + text[used++] - '0';
        if (value > UINT32_MAX) return 0;
    }
    if (!used || !value) return 0;
    while (used < bytes && (text[used] == '\r' || text[used] == '\n' || text[used] == ' ' || text[used] == '\t')) ++used;
    return used == bytes ? (uint32_t)value : 0;
}
uint32_t updateInstalledRevision(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    unsigned char text[32];
    size_t bytes = fread(text, 1, sizeof(text), file);
    bool valid = !ferror(file) && bytes < sizeof(text);
    fclose(file);
    return valid ? revisionText(text, bytes) : 0;
}
bool updateManagedName(const char *name) {
    size_t length = strlen(name);
    if (!length || length >= UPDATE_NAME_BYTES || name[0] == '/' || name[length - 1] == '/') return false;
    for (const char *part = name; *part;) {
        const char *end = strchr(part, '/');
        size_t count = end ? (size_t)(end - part) : strlen(part);
        if (!count || (count == 1 && part[0] == '.') || (count == 2 && !memcmp(part, "..", 2))) return false;
        for (size_t i = 0; i < count; ++i)
            if ((unsigned char)part[i] < 32 || part[i] == 127 || part[i] == ':' || part[i] == '\\') return false;
        part += count;
        if (*part) ++part;
    }
    if (!strcmp(name, "revision.txt") || !strcmp(name, "bin/linux/arm64/PokeMMO")) return true;
    return !strncmp(name, "data/", 5) && strcasecmp(name, "data/mods") && strncasecmp(name, "data/mods/", 10);
}
static bool rangeAt(UpdateHttp *http, uint64_t offset, size_t bytes, const char *etag, void *data, UpdateReply *reply) {
    if (!bytes || offset > UINT64_MAX - bytes) return false;
    char range[64];
    snprintf(range, sizeof(range), "%llu-%llu", (unsigned long long)offset, (unsigned long long)(offset + bytes - 1));
    return updateHttpRange(http, range, etag, data, bytes, reply) && reply->start == offset && reply->bytes == bytes;
}
bool updateRemoteRevision(UpdateHttp *http, UpdateRemote *remote) {
    enum { TAIL = 65557, MAX_DIRECTORY = 1024 * 1024 };
    unsigned char *tail = malloc(TAIL), *directory = NULL, *local = NULL;
    if (!tail) return false;
    bool ok = false;
    UpdateReply reply;
    if (!updateHttpRange(http, "-65557", NULL, tail, TAIL, &reply) || reply.bytes < 22 || reply.total < reply.bytes || reply.start != reply.total - reply.bytes)
        goto done;
    remote->bytes = reply.total;
    strcpy(remote->etag, reply.etag);
    size_t end = (size_t)reply.bytes - 22;
    for (;;) {
        if (dword(tail + end) == 0x06054b50 && end + 22 + word(tail + end + 20) == reply.bytes) break;
        if (!end--) goto done;
    }
    const unsigned char *record = tail + end;
    unsigned count = word(record + 10);
    uint32_t size = dword(record + 12), offset = dword(record + 16);
    if (word(record + 4) || word(record + 6) || word(record + 8) != count || !count || count > 4096 || !size || size > MAX_DIRECTORY ||
        (uint64_t)offset + size != reply.start + end)
        goto done;
    const unsigned char *central;
    if (offset >= reply.start && (uint64_t)offset + size <= reply.start + end)
        central = tail + offset - reply.start;
    else {
        directory = malloc(size);
        if (!directory || !rangeAt(http, offset, size, remote->etag, directory, &reply) || reply.total != remote->bytes) goto done;
        central = directory;
    }
    uint32_t revision_offset = 0, compressed = 0, expanded = 0, checksum = 0;
    unsigned method = 0, found = 0;
    size_t at = 0;
    remote->expanded = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (at > size || size - at < 46 || dword(central + at) != 0x02014b50) goto done;
        const unsigned char *entry = central + at;
        unsigned name_bytes = word(entry + 28);
        size_t span = 46u + name_bytes + word(entry + 30) + word(entry + 32);
        if (span > size - at || word(entry + 34)) goto done;
        if (name_bytes && name_bytes < UPDATE_NAME_BYTES && !memchr(entry + 46, 0, name_bytes)) {
            char name[UPDATE_NAME_BYTES];
            memcpy(name, entry + 46, name_bytes);
            name[name_bytes] = 0;
            if (updateManagedName(name)) {
                if ((word(entry + 8) & 1) || (word(entry + 10) != 0 && word(entry + 10) != 8) || ((dword(entry + 38) >> 16) & 0170000) == 0120000) goto done;
                remote->expanded += dword(entry + 24);
                if (remote->expanded > UPDATE_MAX_EXPANDED) goto done;
            }
            if (!strcmp(name, "revision.txt")) {
                if (++found != 1 || (word(entry + 8) & 1)) goto done;
                method = word(entry + 10);
                checksum = dword(entry + 16);
                compressed = dword(entry + 20);
                expanded = dword(entry + 24);
                revision_offset = dword(entry + 42);
            }
        }
        at += span;
    }
    if (at != size || found != 1 || !compressed || compressed > 128 || !expanded || expanded > 31 || (method != 0 && method != 8) ||
        (uint64_t)revision_offset + 42 > remote->bytes)
        goto done;
    local = malloc(65536 + 512);
    size_t local_bytes = remote->bytes - revision_offset < 512 ? (size_t)(remote->bytes - revision_offset) : 512;
    if (!local || !rangeAt(http, revision_offset, local_bytes, remote->etag, local, &reply) || reply.total != remote->bytes || dword(local) != 0x04034b50 ||
        word(local + 8) != method || word(local + 26) != 12 || memcmp(local + 30, "revision.txt", 12))
        goto done;
    size_t data_offset = 30u + word(local + 26) + word(local + 28);
    if ((uint64_t)revision_offset + data_offset + compressed > remote->bytes) goto done;
    if (data_offset + compressed > local_bytes &&
        (!rangeAt(http, revision_offset, data_offset + compressed, remote->etag, local, &reply) || reply.total != remote->bytes))
        goto done;
    unsigned char text[32] = {0};
    if (method == 0) {
        if (compressed != expanded) goto done;
        memcpy(text, local + data_offset, expanded);
    } else {
        z_stream stream = {.next_in = local + data_offset, .avail_in = compressed, .next_out = text, .avail_out = sizeof(text)};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) goto done;
        int result = inflate(&stream, Z_FINISH);
        bool complete = result == Z_STREAM_END && stream.total_in == compressed && stream.total_out == expanded;
        inflateEnd(&stream);
        if (!complete) goto done;
    }
    if ((uint32_t)crc32(0, text, expanded) != checksum) goto done;
    remote->revision = revisionText(text, expanded);
    ok = remote->revision != 0;
done:
    free(local);
    free(directory);
    free(tail);
    return ok;
}
bool updatePrepare(UpdateJob *job, uint32_t revision, UpdateEntry **out_entries, unsigned *out_count) {
    unzFile archive = unzOpen64(UPDATE_ARCHIVE);
    if (!archive) return false;
    bool ok = false, client_seen = false;
    unz_global_info64 global;
    UpdateEntry *entries = NULL;
    unz64_file_pos *positions = NULL;
    unsigned count = 0, revision_index = 0;
    unsigned char *buffer = malloc(65536);
    if (!buffer || unzGetGlobalInfo64(archive, &global) != UNZ_OK || !global.number_entry || global.number_entry > 4096) goto done;
    entries = calloc(global.number_entry, sizeof(*entries));
    positions = calloc(global.number_entry, sizeof(*positions));
    if (!entries || !positions || unzGoToFirstFile(archive) != UNZ_OK) goto done;
    uint64_t total = 0;
    for (uint64_t i = 0; i < global.number_entry; ++i) {
        unz_file_info64 info;
        char name[UPDATE_NAME_BYTES];
        if (unzGetCurrentFileInfo64(archive, &info, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK || info.size_filename >= sizeof(name) ||
            strlen(name) != info.size_filename)
            goto done;
        if (updateManagedName(name)) {
            if (count >= UPDATE_MAX_FILES || info.uncompressed_size > UPDATE_MAX_EXPANDED || (info.flag & 1) ||
                (info.compression_method != 0 && info.compression_method != 8) || ((info.external_fa >> 16) & 0170000) == 0120000)
                goto done;
            for (unsigned j = 0; j < count; ++j)
                if (!strcasecmp(name, entries[j].name)) goto done;
            UpdateEntry *entry = &entries[count];
            strcpy(entry->name, name);
            entry->bytes = info.uncompressed_size;
            entry->crc = info.crc;
            if (unzGetFilePos64(archive, &positions[count]) != UNZ_OK) goto done;
            char path[512];
            snprintf(path, sizeof(path), UPDATE_GAME "/%s", name);
            struct stat old;
            if (!stat(path, &old)) {
                if (!S_ISREG(old.st_mode)) goto done;
                entry->had_old = 1;
            } else if (errno != ENOENT)
                goto done;
            if (!strcmp(name, "revision.txt")) revision_index = count;
            if (!strcmp(name, "bin/linux/arm64/PokeMMO")) client_seen = true;
            ++count;
            total += entry->bytes;
            if (total > UPDATE_MAX_EXPANDED) goto done;
        }
        if (i + 1 < global.number_entry && unzGoToNextFile(archive) != UNZ_OK) goto done;
    }
    if (!count || !client_seen || strcmp(entries[revision_index].name, "revision.txt") || !total) goto done;
    // revision.txt is the last file installed, after all of the new client's data and executable.
    UpdateEntry last = entries[revision_index];
    unz64_file_pos last_position = positions[revision_index];
    memmove(entries + revision_index, entries + revision_index + 1, (count - revision_index - 1) * sizeof(*entries));
    memmove(positions + revision_index, positions + revision_index + 1, (count - revision_index - 1) * sizeof(*positions));
    entries[count - 1] = last;
    positions[count - 1] = last_position;
    uint64_t done_bytes = 0;
    updateProgress(job, UPDATE_EXTRACT, 700);
    for (unsigned i = 0; i < count; ++i) {
        char path[512];
        snprintf(path, sizeof(path), UPDATE_WORK "/stage/%s", entries[i].name);
        if (updateCancelled(job) || !updateMakeParents(path) || unzGoToFilePos64(archive, &positions[i]) != UNZ_OK || unzOpenCurrentFile(archive) != UNZ_OK)
            goto done;
        FILE *file = fopen(path, "wb");
        bool written = file != NULL;
        uint64_t bytes = 0;
        while (written && !updateCancelled(job)) {
            int got = unzReadCurrentFile(archive, buffer, 65536);
            if (got < 0) {
                written = false;
                break;
            }
            if (!got) break;
            if (bytes + (unsigned)got > entries[i].bytes || fwrite(buffer, 1, got, file) != (size_t)got) {
                written = false;
                break;
            }
            bytes += got;
            updateProgress(job, UPDATE_EXTRACT, 700 + (unsigned)((done_bytes + bytes) * 250 / total));
        }
        if (unzCloseCurrentFile(archive) != UNZ_OK) written = false;
        if (file) {
            if (fflush(file) || fsync(fileno(file))) written = false;
            if (fclose(file)) written = false;
        }
        if (!written || bytes != entries[i].bytes || updateCancelled(job)) goto done;
        done_bytes += bytes;
    }
    ElfLayout layout;
    if (updateInstalledRevision(UPDATE_WORK "/stage/revision.txt") != revision ||
        !elfReadLayout(UPDATE_WORK "/stage/bin/linux/arm64/PokeMMO", &layout, false) || !updateCommit())
        goto done;
    *out_entries = entries;
    *out_count = count;
    entries = NULL;
    ok = true;
done:
    free(buffer);
    free(positions);
    free(entries);
    unzClose(archive);
    return ok;
}
