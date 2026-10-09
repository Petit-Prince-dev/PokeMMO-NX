#include "linux_files.h"
#include "diagnostics.h"
#include "linux_directories.h"
#include "linux_sync.h"
#include "linux_stdio.h"
#include "linux_runtime.h"
#include "linux_net_native.h"
#include "linux_vfd.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

// ---- the SD card's files and folders, through libnx's fsdev ---------------------------------------------------------------------------
// Each operation keeps the caller's errno, returns a Linux error (or sets *error when it returns a value) and traces the first calls and every failure.
_Static_assert(sizeof(off_t) == 8, "native file positions must be 64-bit");
static _Atomic unsigned file_trace_count, dir_trace_count;
static int translateError(int value) {
    switch (value) {
#define ERR(native, linux)                                                                                                                                     \
    case native: return linux
        ERR(EPERM, LINUX_EPERM);
        ERR(ENOENT, LINUX_ENOENT);
        ERR(EINTR, LINUX_EINTR);
        ERR(EBADF, LINUX_EBADF);
        ERR(EAGAIN, LINUX_EAGAIN);
        ERR(ENOMEM, LINUX_ENOMEM);
        ERR(EACCES, LINUX_EACCES);
        ERR(EFAULT, LINUX_EFAULT);
        ERR(EBUSY, LINUX_EBUSY);
        ERR(EEXIST, LINUX_EEXIST);
        ERR(ENOTDIR, LINUX_ENOTDIR);
        ERR(EISDIR, LINUX_EISDIR);
        ERR(EINVAL, LINUX_EINVAL);
        ERR(ENFILE, LINUX_ENFILE);
        ERR(EMFILE, LINUX_EMFILE);
        ERR(EFBIG, LINUX_EFBIG);
        ERR(ENOSPC, LINUX_ENOSPC);
        ERR(ESPIPE, LINUX_ESPIPE);
        ERR(EROFS, LINUX_EROFS);
        ERR(ENAMETOOLONG, LINUX_ENAMETOOLONG);
        ERR(ENOSYS, LINUX_ENOSYS);
        ERR(ENOTEMPTY, LINUX_ENOTEMPTY);
        ERR(EILSEQ, LINUX_EILSEQ);
        ERR(ELOOP, LINUX_ELOOP);
        ERR(EOVERFLOW, LINUX_EOVERFLOW);
        ERR(ENOTSUP, LINUX_ENOTSUP);
#undef ERR
        default: return LINUX_EIO;
    }
}
static void traceFile(const char *operation, int64_t result, int error) {
    unsigned count = atomic_fetch_add_explicit(&file_trace_count, 1, memory_order_relaxed);
    if (count >= 64 && result >= 0) return;
    if (result < 0)
        diagnosticsTrace("files.native.%s=RETURN result=%lld linux_error=%d native_errno=%d fs_rc=0x%x", operation, (long long)result, error, errno,
                         fsdevGetLastResult());
    else
        diagnosticsTrace("files.native.%s=RETURN result=%lld linux_error=%d", operation, (long long)result, error);
}
static int done(const char *operation, int64_t result, int saved, int *error) {
    *error = result < 0 ? translateError(errno) : 0;
    traceFile(operation, result, *error);
    errno = saved;
    return *error;
}
static int nativeOpen(const char *path, int flags, unsigned mode, bool *writable, int *error) {
    // One native read/write handle serves independent logical descriptors.
    // Read-only media can fall back to a genuinely read-only native handle.
    int converted = O_RDWR;
    if (flags & LINUX_O_CREAT) converted |= O_CREAT;
    if (flags & LINUX_O_EXCL) converted |= O_EXCL;
    if (flags & LINUX_O_TRUNC) converted |= O_TRUNC;
    int saved = errno;
    // exec/fork is absent; CLOEXEC stays in the managed descriptor metadata.
    int fd = open(path, converted, (mode_t)(mode & 0777));
    *writable = fd >= 0;
    if (fd < 0 && (flags & 3) == 0) fd = open(path, (converted & ~O_ACCMODE) | O_RDONLY, (mode_t)(mode & 0777));
    done("open", fd, saved, error);
    return fd;
}
static int nativeClose(int fd) {
    int error = 0, saved = errno;
    return done("close", close(fd), saved, &error);
}
static int64_t nativeRead(int fd, void *buffer, size_t count, int *error) {
    int saved = errno;
    int64_t result = 0;
    if (count > INT_MAX) count = INT_MAX;  // partial transfer allowed, never narrow a huge count
    if (count) result = read(fd, buffer, count);
    done("read", result, saved, error);
    return result;
}
static int64_t nativeWrite(int fd, const void *buffer, size_t count, int *error) {
    int saved = errno;
    int64_t result = 0;
    if (count > INT_MAX) count = INT_MAX;
    if (count) result = write(fd, buffer, count);
    done("write", result, saved, error);
    return result;
}
static int64_t nativeSeek(int fd, int64_t offset, int whence, int *error) {
    int saved = errno;
    int64_t result = lseek(fd, offset, whence);
    done("seek", result, saved, error);
    return result;
}
static int nativeFlush(int fd) {
    int error = 0, saved = errno;
    return done("flush", fsync(fd), saved, &error);
}
static int nativeTruncate(int fd, int64_t size) {
    int error = 0, saved = errno;
    return done("truncate", ftruncate(fd, size), saved, &error);
}
static int convert(const struct stat *s, LinuxFileStat *output) {
    if ((__int128)s->st_blksize > INT32_MAX || (__int128)s->st_blksize < INT32_MIN) return LINUX_EOVERFLOW;
    LinuxFileStat value = {0};
    value.device = s->st_dev;
    value.inode = s->st_ino;
    value.mode = s->st_mode;
    value.links = s->st_nlink;
    value.uid = s->st_uid;
    value.gid = s->st_gid;
    value.rdevice = s->st_rdev;
    value.size = s->st_size;
    value.block_size = s->st_blksize;
    value.blocks = s->st_blocks;
    value.access_time = (LinuxTimespec){s->st_atim.tv_sec, s->st_atim.tv_nsec};
    value.modify_time = (LinuxTimespec){s->st_mtim.tv_sec, s->st_mtim.tv_nsec};
    value.change_time = (LinuxTimespec){s->st_ctim.tv_sec, s->st_ctim.tv_nsec};
    *output = value;
    return 0;
}
static int nativeStat(const char *path, bool follow, LinuxFileStat *output) {
    int error = 0, saved = errno;
    struct stat native;
    int result = follow ? stat(path, &native) : lstat(path, &native);  // SD has no symlinks
    done("stat", result, saved, &error);
    return error ? error : convert(&native, output);
}
static int nativeFstat(int fd, LinuxFileStat *output) {
    int error = 0, saved = errno;
    struct stat native;
    done("fstat", fstat(fd, &native), saved, &error);
    return error ? error : convert(&native, output);
}
static int nativeUnlink(const char *path) {
    int error = 0, saved = errno;
    return done("unlink", unlink(path), saved, &error);
}
static int nativeRename(const char *old_path, const char *new_path) {
    int error = 0, saved = errno;
    return done("rename", rename(old_path, new_path), saved, &error);
}

static int traceDir(const char *operation, int error, bool end) {
    unsigned count = atomic_fetch_add_explicit(&dir_trace_count, 1, memory_order_relaxed);
    if (count < 80 || error) {
        if (error)
            diagnosticsTrace("dirs.native.%s=RETURN linux_error=%d end=%u native_errno=%d fs_rc=0x%x", operation, error, end, errno, fsdevGetLastResult());
        else
            diagnosticsTrace("dirs.native.%s=RETURN linux_error=%d end=%u", operation, error, end);
    }
    return error;
}
// libnx's small errno table maps Fs DirectoryNotEmpty (module 2, description 8) to EIO. Translate that exact rmdir result only.
static int rmdirError(int native_error, uint32_t fs_result) {
    if (native_error == EIO && fs_result == 0x1002) return LINUX_ENOTEMPTY;
    return translateError(native_error);
}
// Type of a path (folder or regular file) asked of the file system directly: finds names that a listing omitted.
static int nativeLookup(const char *name, LinuxDirectoryEntry *value) {
    int saved = errno, error = 0;
    char path[FS_MAX_PATH];
    FsFileSystem *fs = NULL;
    if (fsdevTranslatePath(name, &fs, path))
        error = translateError(errno);
    else if (!fs)
        error = LINUX_EIO;
    else {
        FsDirEntryType type = (FsDirEntryType)-1;
        Result rc = fsFsGetEntryType(fs, path, &type);
        if (R_FAILED(rc))
            error = R_MODULE(rc) == 2 && R_DESCRIPTION(rc) == 1 ? LINUX_ENOENT : LINUX_EIO;
        else if (type != FsDirEntryType_Dir && type != FsDirEntryType_File)
            error = LINUX_EIO;
        else {
            value->inode = 0;
            value->type = type == FsDirEntryType_Dir ? 4 : 8;
        }
    }
    errno = saved;
    return error;
}
static void *nativeDirOpen(const char *name, int *error) {
    int saved = errno;
    *error = 0;
    void *result = opendir(name);
    if (!result) *error = translateError(errno);
    traceDir("open", *error, false);
    errno = saved;
    return result;
}
static int nativeDirRead(void *native, LinuxDirectoryEntry *output, bool *end) {
    int saved = errno, error = 0;
    *end = false;
    LinuxDirectoryEntry value = {0};
    errno = 0;
    struct dirent *entry = readdir(native);
    // libnx fsdev uses ENOENT to signal the end of a directory.
    if (!entry) {
        if (!errno || errno == ENOENT)
            *end = true;
        else
            error = translateError(errno);
    } else {
        size_t length = strnlen(entry->d_name, sizeof(value.name));
        if (length == sizeof(value.name))
            error = LINUX_ENAMETOOLONG;
        else {
            memcpy(value.name, entry->d_name, length + 1);
            value.inode = entry->d_ino;
            value.type = entry->d_type;
        }
    }
    if (!error && !*end) *output = value;
    traceDir("read", error, *end);
    errno = saved;
    return error;
}
static int nativeDirClose(void *native, bool *released) {
    int saved = errno;
    // newlib consumes the DIR allocation even on a close error.
    int result = closedir(native);
    *released = true;
    int error = result ? translateError(errno) : 0;
    traceDir("close", error, false);
    errno = saved;
    return error;
}
static int nativeMkdir(const char *name, unsigned mode) {
    int saved = errno, error = 0;
    if (mkdir(name, (mode_t)mode)) error = translateError(errno);
    traceDir("mkdir", error, false);
    errno = saved;
    return error;
}
static int nativeRmdir(const char *name) {
    int saved = errno, error = 0;
    if (rmdir(name)) error = rmdirError(errno, fsdevGetLastResult());
    traceDir("rmdir", error, false);
    errno = saved;
    return error;
}
_Static_assert(sizeof(LinuxFileStat) == 128 && _Alignof(LinuxFileStat) == 8, "glibc AArch64 stat");
_Static_assert(offsetof(LinuxFileStat, mode) == 16 && offsetof(LinuxFileStat, size) == 48 && offsetof(LinuxFileStat, blocks) == 64 &&
                   offsetof(LinuxFileStat, access_time) == 72 && offsetof(LinuxFileStat, reserved) == 120,
               "glibc stat offsets");
typedef struct {
    bool active, native_writable, delete_on_close;
    int native, flags, virtual_kind;
    int64_t offset;
    uint64_t generation;
    char path[LINUX_FILE_PATH_MAX];
} Descriptor;
static Descriptor descriptors[LINUX_FILE_MAX_DESCRIPTORS];
static uint64_t next_generation = 1;
static char root[LINUX_FILE_PATH_MAX];
static char cwd[LINUX_FILE_PATH_MAX];  // empty means virtual /; never changes native cwd
typedef struct {
    bool seen;
    char name[LINUX_FILE_PATH_MAX];
} ObservedEntry;
typedef struct {
    bool active, native_end;
    void *native;
    LinuxDirectoryEntry entry;
    int64_t position;
    char path[LINUX_FILE_PATH_MAX];
    ObservedEntry *observed;
    size_t observed_count, observed_next;
} Directory;
static Directory directories[LINUX_DIRECTORY_MAX];
// Some Horizon SD listings omit accessible non-ASCII names. Record successful
// path observations, not expected names. Each stream takes a snapshot
// and rechecks existence before supplementing its native enumeration.
static char observed_paths[LINUX_DIRECTORY_OBSERVED_MAX][LINUX_FILE_PATH_MAX];
static bool observed_overflow;
_Static_assert(sizeof(LinuxDirectoryEntry) == 280 && _Alignof(LinuxDirectoryEntry) == 8, "glibc dirent64 layout");
_Static_assert(offsetof(LinuxDirectoryEntry, offset) == 8 && offsetof(LinuxDirectoryEntry, record_bytes) == 16 && offsetof(LinuxDirectoryEntry, type) == 18 &&
                   offsetof(LinuxDirectoryEntry, name) == 19,
               "glibc dirent64 fields");
static int fail(int error) {
    *linuxAbiErrnoLocation() = error;
    return -1;
}
static int finish(int error) {
    linuxSyncUnlock();
    return error ? fail(error) : 0;
}
static Descriptor *get(int fd) {
    return fd >= 3 && (unsigned)(fd - 3) < LINUX_FILE_MAX_DESCRIPTORS && descriptors[fd - 3].active ? &descriptors[fd - 3] : NULL;
}
static Descriptor *byPath(const char *name) {
    for (unsigned i = 0; i < LINUX_FILE_MAX_DESCRIPTORS; ++i)
        if (descriptors[i].active && !strcmp(descriptors[i].path, name)) return &descriptors[i];
    return NULL;
}
static bool pathPrefix(const char *name, const char *prefix) {
    size_t length = strlen(prefix);
    return !strncmp(name, prefix, length) && (!name[length] || name[length] == '/');
}
static void observePath(const char *name) {
    const unsigned char *base = (const unsigned char *)strrchr(name, '/');
    base = base ? base + 1 : (const unsigned char *)name;
    bool unicode = false;
    for (const unsigned char *p = base; *p; ++p)
        if (*p >= 128) unicode = true;
    if (!unicode) return;
    unsigned available = LINUX_DIRECTORY_OBSERVED_MAX;
    for (unsigned i = 0; i < LINUX_DIRECTORY_OBSERVED_MAX; ++i) {
        if (!strcmp(observed_paths[i], name)) return;
        if (!observed_paths[i][0] && available == LINUX_DIRECTORY_OBSERVED_MAX) available = i;
    }
    if (available == LINUX_DIRECTORY_OBSERVED_MAX)
        observed_overflow = true;
    else
        strcpy(observed_paths[available], name);
}
static void forgetPath(const char *name) {
    for (unsigned i = 0; i < LINUX_DIRECTORY_OBSERVED_MAX; ++i)
        if (observed_paths[i][0] && pathPrefix(observed_paths[i], name)) observed_paths[i][0] = 0;
}
static void renameObserved(const char *old_path, const char *new_path) {
    if (!strcmp(old_path, new_path)) return;
    forgetPath(new_path);
    size_t old_length = strlen(old_path), new_length = strlen(new_path);
    for (unsigned i = 0; i < LINUX_DIRECTORY_OBSERVED_MAX; ++i)
        if (observed_paths[i][0] && pathPrefix(observed_paths[i], old_path)) {
            char moved[LINUX_FILE_PATH_MAX];
            size_t suffix = strlen(observed_paths[i] + old_length);
            if (new_length + suffix >= sizeof(moved)) {
                observed_paths[i][0] = 0;
                observed_overflow = true;
                continue;
            }
            memcpy(moved, new_path, new_length);
            memcpy(moved + new_length, observed_paths[i] + old_length, suffix + 1);
            observed_paths[i][0] = 0;
            observePath(moved);
        }
    observePath(new_path);
}
static const char *observedChild(const char *name, const char *parent) {
    size_t length = strlen(parent);
    if (strncmp(name, parent, length) || name[length] != '/') return NULL;
    const char *base = name + length + 1;
    return *base && !strchr(base, '/') ? base : NULL;
}
static void directoryClear(Directory *d) {
    free(d->observed);
    memset(d, 0, sizeof(*d));
}
// Virtual nodes: /dev holds the three devices a Linux program expects. Their "native" path starts with a
// marker byte so no platform call ever sees them.
enum {
    VIRTUAL_NONE = 0,
    VIRTUAL_DIR = 1,
    VIRTUAL_RANDOM = 2,
    VIRTUAL_NULL = 3,
    VIRTUAL_SOCKET = 4,
    VIRTUAL_PIPE = LINUX_VFD_PIPE,
    VIRTUAL_EPOLL = LINUX_VFD_EPOLL
};
#define IN_MEMORY_KIND(kind) ((kind) >= LINUX_VFD_EVENTFD && (kind) <= LINUX_VFD_EPOLL)
#define VIRTUAL_MARK '\x01'
static int virtualKind(const char *normalized) {
    if (!strcmp(normalized, "/dev")) return VIRTUAL_DIR;
    if (!strcmp(normalized, "/dev/urandom") || !strcmp(normalized, "/dev/random")) return VIRTUAL_RANDOM;
    if (!strcmp(normalized, "/dev/null")) return VIRTUAL_NULL;
    return VIRTUAL_NONE;
}
static int nativeVirtual(const char *name) { return name[0] == VIRTUAL_MARK ? virtualKind(name + 1) : VIRTUAL_NONE; }
static void virtualStat(int kind, LinuxFileStat *output) {
    memset(output, 0, sizeof(*output));
    output->inode = (uint64_t)kind + 1;
    output->links = 1;
    output->block_size = 4096;
    output->mode =
        kind == VIRTUAL_DIR
            ? 040755u
            : (kind == VIRTUAL_SOCKET
                   ? 0140666u
                   : (kind == VIRTUAL_PIPE ? 010600u : (IN_MEMORY_KIND(kind) ? 0600u : 020666u)));  // S_IFDIR / S_IFSOCK / S_IFIFO / anonymous / S_IFCHR
    if (kind != VIRTUAL_DIR && kind != VIRTUAL_SOCKET && !IN_MEMORY_KIND(kind)) output->rdevice = kind == VIRTUAL_NULL ? 0x103 : 0x109;
}
// The file system of the console gives every file the same device and inode (0). A program that tells files apart by them (Java's ZipFile hands
// out the archive it already has open for another file with the same device, inode and modification time, and the game's archives all carry the
// same time) would take different files for one. The inode is made from the path.
static void identify(const char *path, LinuxFileStat *output) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) hash = (hash ^ *p) * 0x100000001b3ull;
    output->device = 1;
    output->inode = hash ? hash : 1;
}
static int statNative(const char *name, bool follow, LinuxFileStat *output) {
    int kind = nativeVirtual(name);
    if (kind) {
        virtualStat(kind, output);
        return 0;
    }
    Descriptor *d = byPath(name);
    int error = d ? nativeFstat(d->native, output) : nativeStat(name, follow, output);
    if (!error) {
        identify(name, output);
        observePath(name);
    }
    return error;
}
static int pending_socket_close = -1;  // a socket is closed after the descriptor lock is released: closing may wait for the network
static int pending_vfd_kind, pending_vfd_object = -1, pending_vfd_access;
static int closeRecord(Descriptor *d) {
    if (d->virtual_kind == VIRTUAL_SOCKET) pending_socket_close = d->native;
    if (IN_MEMORY_KIND(d->virtual_kind)) {
        pending_vfd_kind = d->virtual_kind;
        pending_vfd_object = d->native;
        pending_vfd_access = d->flags & 3;
    }
    if (d->virtual_kind) {
        memset(d, 0, sizeof(*d));
        return 0;
    }
    bool shared = false;
    for (unsigned i = 0; i < LINUX_FILE_MAX_DESCRIPTORS; ++i)
        if (&descriptors[i] != d && descriptors[i].active && descriptors[i].native == d->native) shared = true;
    int error = shared ? 0 : nativeClose(d->native);
    if (!error) {
        // Linux lets a file be unlinked while open; Horizon refuses. The unlink was deferred until the last close.
        char doomed[LINUX_FILE_PATH_MAX];
        doomed[0] = 0;
        if (d->delete_on_close && d->path[0]) {
            bool in_use = false;
            for (unsigned i = 0; i < LINUX_FILE_MAX_DESCRIPTORS; ++i)
                if (&descriptors[i] != d && descriptors[i].active && !strcmp(descriptors[i].path, d->path)) in_use = true;
            if (!in_use) strcpy(doomed, d->path);
        }
        memset(d, 0, sizeof(*d));
        if (doomed[0] && !nativeUnlink(doomed)) forgetPath(doomed);
    }
    return error;
}
// SD SetSize/write growth does not establish the Linux zero-extension contract.
// Fill the newly exposed range explicitly; logical cursors belong to descriptors.
static int zeroRange(int native, int64_t begin, int64_t end) {
    static const unsigned char zeros[4096];
    if (begin >= end) return 0;
    int error = 0;
    nativeSeek(native, begin, 0, &error);
    while (!error && begin < end) {
        size_t count = (uint64_t)(end - begin) > sizeof(zeros) ? sizeof(zeros) : (size_t)(end - begin);
        int64_t written = nativeWrite(native, zeros, count, &error);
        if (!error && (written <= 0 || (uint64_t)written > count)) error = LINUX_EIO;
        if (!error) begin += written;
    }
    return error;
}
static int resize(Descriptor *d, int64_t size) {
    LinuxFileStat old;
    int error = nativeFstat(d->native, &old);
    if (!error && size > old.size) error = zeroRange(d->native, old.size, size);
    if (!error) error = nativeTruncate(d->native, size);
    return error;
}
// Called with metadata held. The same lock serializes file positions and closes
// across all managed descriptors. No managed callback executes under this lock.
static int pathEx(const char *name, char output[LINUX_FILE_PATH_MAX], bool check_trailing) {
    if (!name) return LINUX_EFAULT;
    if (!*name) return LINUX_ENOENT;
    if (!*root) return LINUX_ENOSYS;
    char normalized[LINUX_FILE_PATH_MAX];
    size_t length = name[0] == '/' ? 0 : strlen(cwd);
    memcpy(normalized, cwd, length);
    size_t scan = 0;
    while (name[scan]) {
        if (scan >= LINUX_FILE_PATH_MAX - 1) return LINUX_ENAMETOOLONG;
        if (name[scan] == ':' || name[scan] == '\\') return LINUX_EINVAL;
        ++scan;
    }
    bool directory_required = name[scan - 1] == '/';
    while (*name) {
        while (*name == '/') ++name;
        const char *begin = name;
        while (*name && *name != '/') ++name;
        size_t count = (size_t)(name - begin);
        if (!count || (count == 1 && begin[0] == '.')) continue;
        if (count == 2 && begin[0] == '.' && begin[1] == '.') {
            while (length && normalized[--length] != '/') {}
            continue;
        }
        if (length + 1 + count >= sizeof(normalized)) return LINUX_ENAMETOOLONG;
        normalized[length++] = '/';
        memcpy(normalized + length, begin, count);
        length += count;
    }
    normalized[length] = 0;
    if (virtualKind(normalized)) {
        if (length + 2 >= LINUX_FILE_PATH_MAX) return LINUX_ENAMETOOLONG;
        output[0] = VIRTUAL_MARK;
        memcpy(output + 1, normalized, length + 1);
        return 0;
    }
    for (const char **item = (const char *[]){"/proc", "/sys", "/dev", NULL}; *item; ++item) {
        size_t size = strlen(*item);
        if (!strncmp(normalized, *item, size) && (!normalized[size] || normalized[size] == '/')) return LINUX_ENOSYS;
    }
    // The SD card is the folder /sd of the game: a file chooser can then reach ROMs kept anywhere on it. Everything else lives under root.
    const char *base = root, *tail = normalized;
    size_t tail_length = length;
    if (!strncmp(normalized, "/sd", 3) && (!normalized[3] || normalized[3] == '/')) {
        base = "sdmc:";
        tail = normalized[3] ? normalized + 3 : "/";
        tail_length = strlen(tail);
    }
    size_t prefix = strlen(base);
    if (prefix + tail_length >= LINUX_FILE_PATH_MAX) return LINUX_ENAMETOOLONG;
    memcpy(output, base, prefix);
    memcpy(output + prefix, tail, tail_length + 1);
    if (check_trailing && directory_required && length) {
        LinuxFileStat value;
        int error = statNative(output, true, &value);
        if (error) return error;
        if ((value.mode & 0170000) != 0040000) return LINUX_ENOTDIR;
    }
    return 0;
}
static int path(const char *name, char output[LINUX_FILE_PATH_MAX]) { return pathEx(name, output, true); }
static const char *relativeToRoot(const char *native_path) {  // NULL for a path outside the game's folder (the SD card mount)
    size_t prefix = strlen(root);
    return strncmp(native_path, root, prefix) == 0 ? native_path + prefix : NULL;
}
static bool cwdAncestor(const char *native_path) {
    const char *relative = relativeToRoot(native_path);
    if (!relative) return false;
    size_t length = strlen(relative);
    return !length || (!strncmp(cwd, relative, length) && (!cwd[length] || cwd[length] == '/'));
}
bool linuxFilesSetRoot(const char *name) {
    if (!name || !*name || strlen(name) >= sizeof(root)) return false;
    linuxSyncLock();
    for (unsigned i = 0; i < LINUX_FILE_MAX_DESCRIPTORS; ++i)
        if (descriptors[i].active) {
            linuxSyncUnlock();
            return false;
        }
    for (unsigned i = 0; i < LINUX_DIRECTORY_MAX; ++i)
        if (directories[i].active) {
            linuxSyncUnlock();
            return false;
        }
    strcpy(root, name);
    size_t length = strlen(root);
    while (length > 1 && root[length - 1] == '/') root[--length] = 0;
    cwd[0] = 0;
    memset(observed_paths, 0, sizeof(observed_paths));
    observed_overflow = false;
    linuxSyncUnlock();
    return true;
}
int linuxAbiOpen(const char *name, int flags, ...) {
    unsigned supported = 3 | LINUX_O_CREAT | LINUX_O_EXCL | LINUX_O_NOCTTY | LINUX_O_TRUNC | LINUX_O_APPEND | LINUX_O_NONBLOCK | LINUX_O_CLOEXEC;
    if ((flags & 3) == 3 || ((flags & LINUX_O_TRUNC) && (flags & 3) == 0)) return fail(LINUX_EINVAL);
    if ((unsigned)flags & ~supported) return fail(LINUX_ENOSYS);
    unsigned mode = 0;
    if (flags & LINUX_O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, unsigned);
        va_end(args);
    }
    linuxSyncLock();
    char native_path[LINUX_FILE_PATH_MAX];
    int error = path(name, native_path);
    if (error) return finish(error);
    unsigned index = 0;
    while (index < LINUX_FILE_MAX_DESCRIPTORS && descriptors[index].active) ++index;
    if (index == LINUX_FILE_MAX_DESCRIPTORS) return finish(LINUX_EMFILE);  // before O_TRUNC/O_CREAT effects
    if (!next_generation) return finish(LINUX_EOVERFLOW);
    int kind = nativeVirtual(native_path);
    if (kind) {
        if (kind == VIRTUAL_DIR) return finish(LINUX_EISDIR);
        if (flags & LINUX_O_CREAT && flags & LINUX_O_EXCL) return finish(LINUX_EEXIST);
        descriptors[index] =
            (Descriptor){.active = true, .native_writable = true, .native = -1, .flags = flags, .virtual_kind = kind, .generation = next_generation++};
        strcpy(descriptors[index].path, native_path);
        linuxSyncUnlock();
        return (int)index + 3;
    }
    Descriptor *shared = byPath(native_path);
    if (shared && (flags & LINUX_O_CREAT) && (flags & LINUX_O_EXCL)) return finish(LINUX_EEXIST);
    if (shared && (flags & 3) != 0 && !shared->native_writable) return finish(LINUX_EACCES);
    bool writable = false;
    int native;
    if (shared) {
        native = shared->native;
        writable = shared->native_writable;
        if (flags & LINUX_O_TRUNC) error = resize(shared, 0);
    } else
        native = nativeOpen(native_path, flags, mode, &writable, &error);
    if (error) return finish(error);
    descriptors[index] = (Descriptor){.active = true, .native_writable = writable, .native = native, .flags = flags, .generation = next_generation++};
    strcpy(descriptors[index].path, native_path);
    observePath(native_path);
    linuxSyncUnlock();
    return (int)index + 3;
}
int linuxFileStreamAttach(int fd, bool readable, bool writable, bool append, uint64_t *generation) {
    linuxSyncLock();
    Descriptor *d = get(fd);
    if (!d) return finish(LINUX_EBADF);
    if ((readable && (d->flags & 3) == 1) || (writable && (d->flags & 3) == 0)) return finish(LINUX_EINVAL);
    if (append) d->flags |= LINUX_O_APPEND;
    *generation = d->generation;
    return finish(0);
}
static int closeGeneration(int fd, uint64_t generation) {
    linuxSyncLock();
    Descriptor *d = get(fd);
    if (!d || (generation && d->generation != generation)) return finish(LINUX_EBADF);
    int error = closeRecord(d), socket_handle = pending_socket_close;
    pending_socket_close = -1;
    int vfd_kind = pending_vfd_kind, vfd_object = pending_vfd_object, vfd_access = pending_vfd_access;
    pending_vfd_object = -1;
    int result = finish(error);
    if (socket_handle >= 0) linuxNetNativeClose(socket_handle);
    if (vfd_object >= 0) linuxVfdRelease(vfd_kind, vfd_object, vfd_access);
    return result;
}
int linuxAbiClose(int fd) { return closeGeneration(fd, 0); }
int linuxFileStreamClose(int fd, uint64_t generation) { return closeGeneration(fd, generation); }
static int64_t io(int fd, void *buffer, size_t count, bool write, uint64_t generation) {
    if (count > INT64_MAX) return fail(LINUX_EINVAL);
    if (count && !buffer) return fail(LINUX_EFAULT);
    linuxSyncLock();
    Descriptor *d = get(fd);
    if (!d || (generation && d->generation != generation) || (write ? (d->flags & 3) == 0 : (d->flags & 3) == 1)) return finish(LINUX_EBADF);

    if (d->virtual_kind == VIRTUAL_SOCKET) {
        // A socket may block for a long time: never hold the descriptor lock meanwhile.
        int handle = d->native, socket_error = 0;
        linuxSyncUnlock();
        int64_t transferred = count ? (write ? linuxNetNativeSend(handle, buffer, count, 0, NULL, &socket_error)
                                             : linuxNetNativeRecv(handle, buffer, count, 0, NULL, &socket_error))
                                    : 0;
        return transferred < 0 ? fail(socket_error) : transferred;
    }
    if (IN_MEMORY_KIND(d->virtual_kind)) {
        int kind = d->virtual_kind, object = d->native, vfd_error = 0;
        bool nonblock = (d->flags & LINUX_O_NONBLOCK) != 0;
        linuxSyncUnlock();
        if (kind == VIRTUAL_EPOLL) return fail(LINUX_EINVAL);
        int64_t transferred =
            write ? linuxVfdWrite(kind, object, buffer, count, nonblock, &vfd_error) : linuxVfdRead(kind, object, buffer, count, nonblock, &vfd_error);
        return transferred < 0 ? fail(vfd_error) : transferred;
    }
    if (d->virtual_kind) {
        int vk = d->virtual_kind;
        if (!write && vk == VIRTUAL_RANDOM && count) randomGet(buffer, count);
        finish(0);
        return vk == VIRTUAL_NULL && !write ? 0 : (int64_t)count;
    }
    int error = 0;
    int64_t result = 0, position = d->offset;
    if (count && write) {
        LinuxFileStat value;
        error = nativeFstat(d->native, &value);
        if (!error && (d->flags & LINUX_O_APPEND)) position = value.size;
        size_t transfer = count > INT_MAX ? INT_MAX : count;
        if (!error && position > INT64_MAX - (int64_t)transfer) error = LINUX_EINVAL;
        if (!error) error = zeroRange(d->native, value.size, position);
    }
    if (count && !error) nativeSeek(d->native, position, 0, &error);
    if (count && !error) result = write ? nativeWrite(d->native, buffer, count, &error) : nativeRead(d->native, buffer, count, &error);
    if (!error) d->offset = position + result;
    finish(error);
    return error ? -1 : result;
}
int64_t linuxAbiRead(int fd, void *buffer, size_t count) { return io(fd, buffer, count, false, 0); }
int64_t linuxAbiWrite(int fd, const void *buffer, size_t count) { return io(fd, (void *)buffer, count, true, 0); }
int64_t linuxFileStreamRead(int fd, uint64_t generation, void *buffer, size_t count) { return io(fd, buffer, count, false, generation); }
int64_t linuxFileStreamWrite(int fd, uint64_t generation, const void *buffer, size_t count) { return io(fd, (void *)buffer, count, true, generation); }
static int64_t seekGeneration(int fd, int64_t offset, int whence, uint64_t generation) {
    if (whence < 0 || whence > 2) return fail(LINUX_EINVAL);
    linuxSyncLock();
    Descriptor *d = get(fd);
    if (!d || (generation && d->generation != generation)) return finish(LINUX_EBADF);
    if (d->virtual_kind) return finish(LINUX_ESPIPE);
    int error = 0;
    int64_t base = 0, result = -1;
    if (whence == 1) base = d->offset;
    if (whence == 2) {
        LinuxFileStat value;
        error = nativeFstat(d->native, &value);
        if (!error) base = value.size;
    }
    __int128 destination = (__int128)base + offset;
    if (!error && (destination < 0 || destination > INT64_MAX)) error = LINUX_EINVAL;
    if (!error) result = nativeSeek(d->native, (int64_t)destination, 0, &error);
    if (!error) d->offset = result;
    finish(error);
    return error ? -1 : result;
}
int64_t linuxAbiLseek(int fd, int64_t offset, int whence) { return seekGeneration(fd, offset, whence, 0); }
int64_t linuxFileStreamSeek(int fd, uint64_t generation, int64_t offset, int whence) { return seekGeneration(fd, offset, whence, generation); }
static int64_t positional(int fd, void *buffer, size_t count, int64_t offset, bool write) {
    if (offset < 0 || count > INT64_MAX || count > (uint64_t)(INT64_MAX - offset)) return fail(LINUX_EINVAL);
    if (count && !buffer) return fail(LINUX_EFAULT);
    linuxSyncLock();
    Descriptor *d = get(fd);
    if (!d || (write ? (d->flags & 3) == 0 : (d->flags & 3) == 1)) return finish(LINUX_EBADF);
    if (d->virtual_kind) return finish(LINUX_ESPIPE);
    if (write && (d->flags & LINUX_O_APPEND)) return finish(LINUX_ENOSYS);

    int error = 0, restore_error = 0;
    int64_t result = -1;
    int64_t saved = d->offset;
    if (count && write) {
        LinuxFileStat value;
        error = nativeFstat(d->native, &value);
        if (!error) error = zeroRange(d->native, value.size, offset);
    }
    if (!error) nativeSeek(d->native, saved, 0, &error);
    if (!error) nativeSeek(d->native, offset, 0, &error);
    if (!error) {
        result = write ? nativeWrite(d->native, buffer, count, &error) : nativeRead(d->native, buffer, count, &error);
        nativeSeek(d->native, saved, 0, &restore_error);
        if (!error) error = restore_error;
    }
    finish(error);
    return error ? -1 : result;
}
int64_t linuxAbiPread(int fd, void *buffer, size_t count, int64_t offset) { return positional(fd, buffer, count, offset, false); }
int64_t linuxAbiPwrite(int fd, const void *buffer, size_t count, int64_t offset) { return positional(fd, (void *)buffer, count, offset, true); }
int linuxAbiFsync(int fd) {
    linuxSyncLock();
    Descriptor *d = get(fd);
    if (!d) return finish(LINUX_EBADF);
    if (d->virtual_kind) return finish(LINUX_EINVAL);
    return finish(nativeFlush(d->native));
}
int linuxAbiFtruncate(int fd, int64_t size) {
    if (size < 0) return fail(LINUX_EINVAL);
    linuxSyncLock();
    Descriptor *d = get(fd);
    if (!d || (d->flags & 3) == 0) return finish(LINUX_EBADF);
    if (d->virtual_kind) return finish(LINUX_EINVAL);
    return finish(resize(d, size));
}
static int statPath(int version, const char *name, LinuxFileStat *output, bool follow) {
    if (version != 0) return fail(LINUX_EINVAL);
    if (!output) return fail(LINUX_EFAULT);
    linuxSyncLock();
    char native_path[LINUX_FILE_PATH_MAX];
    int error = path(name, native_path);
    LinuxFileStat value;
    if (!error) { error = statNative(native_path, follow, &value); }
    if (!error) *output = value;
    return finish(error);
}
int linuxAbiXstat(int version, const char *name, LinuxFileStat *output) { return statPath(version, name, output, true); }
int linuxAbiLxstat(int version, const char *name, LinuxFileStat *output) { return statPath(version, name, output, false); }
int linuxAbiFxstat(int version, int fd, LinuxFileStat *output) {
    if (version != 0) return fail(LINUX_EINVAL);
    if (!output) return fail(LINUX_EFAULT);
    linuxSyncLock();
    Descriptor *d = get(fd);
    if (!d) return finish(LINUX_EBADF);
    LinuxFileStat value;
    int error = 0;
    if (d->virtual_kind)
        virtualStat(d->virtual_kind, &value);
    else {
        error = nativeFstat(d->native, &value);
        if (!error) identify(d->path, &value);
    }
    if (!error) *output = value;
    return finish(error);
}
// access(2): existence is the whole check (the SD card has no permission model); R_OK/W_OK/X_OK values are validated.
// The SD card path behind a guest path, for the loader (it maps libraries from files).
int linuxFilesNativePath(const char *name, char output[LINUX_FILE_PATH_MAX]) {
    linuxSyncLock();
    int error = path(name, output);
    if (!error && nativeVirtual(output)) error = LINUX_ENOENT;  // devices have no file behind them
    return finish(error);
}
int linuxAbiAccess(const char *name, int mode) {
    if (mode & ~7) return fail(LINUX_EINVAL);
    linuxSyncLock();
    char native_path[LINUX_FILE_PATH_MAX];
    int error = path(name, native_path);
    LinuxFileStat value;
    if (!error) { error = statNative(native_path, true, &value); }
    return finish(error);
}
int linuxFilesVirtualCreate(int kind, int object, int flags) {
    linuxSyncLock();
    unsigned index = 0;
    while (index < LINUX_FILE_MAX_DESCRIPTORS && descriptors[index].active) ++index;
    if (index == LINUX_FILE_MAX_DESCRIPTORS) return finish(LINUX_EMFILE);
    if (!next_generation) return finish(LINUX_EOVERFLOW);
    descriptors[index] =
        (Descriptor){.active = true, .native_writable = true, .native = object, .flags = flags, .virtual_kind = kind, .generation = next_generation++};

    linuxSyncUnlock();
    return (int)index + 3;
}
bool linuxFilesVirtualPeek(int fd, int *kind, int *object, int *flags) {
    linuxSyncLock();
    Descriptor *d = get(fd);
    bool match = d && IN_MEMORY_KIND(d->virtual_kind);
    if (match) {
        *kind = d->virtual_kind;
        *object = d->native;
        *flags = d->flags;
    }
    linuxSyncUnlock();
    return match;
}
int linuxFilesSocketCreate(int handle, int flags) {
    linuxSyncLock();
    unsigned index = 0;
    while (index < LINUX_FILE_MAX_DESCRIPTORS && descriptors[index].active) ++index;
    if (index == LINUX_FILE_MAX_DESCRIPTORS) return finish(LINUX_EMFILE);
    if (!next_generation) return finish(LINUX_EOVERFLOW);
    descriptors[index] = (Descriptor){
        .active = true, .native_writable = true, .native = handle, .flags = flags, .virtual_kind = VIRTUAL_SOCKET, .generation = next_generation++};

    linuxSyncUnlock();
    return (int)index + 3;
}
bool linuxFilesSocketPeek(int fd, int *handle, int *flags) {
    linuxSyncLock();
    Descriptor *d = get(fd);
    bool socket = d && d->virtual_kind == VIRTUAL_SOCKET;
    if (socket) {
        *handle = d->native;
        *flags = d->flags;
    }
    linuxSyncUnlock();
    return socket;
}
bool linuxFilesSocketLookup(int fd, int *handle, int *flags) {
    linuxSyncLock();
    Descriptor *d = get(fd);
    if (!d) {
        finish(LINUX_EBADF);
        return false;
    }
    if (d->virtual_kind != VIRTUAL_SOCKET) {
        finish(88);
        return false;
    }  // ENOTSOCK
    *handle = d->native;
    *flags = d->flags;
    linuxSyncUnlock();
    return true;
}
bool linuxFilesDescriptorInfo(int fd, char output[LINUX_FILE_PATH_MAX], int *flags) {
    linuxSyncLock();
    Descriptor *d = get(fd);
    if (!d) {
        finish(LINUX_EBADF);
        return false;
    }
    if (d->virtual_kind || !d->path[0]) {
        finish(19);
        return false;
    }  // ENODEV: not a regular file
    strcpy(output, d->path);
    *flags = d->flags;
    linuxSyncUnlock();
    return true;
}
int linuxFilesWriteBack(const char *native_path, int64_t offset, const void *data, size_t count) {
    linuxSyncLock();
    Descriptor *d = byPath(native_path);
    int native, error = 0;
    bool temporary = false;
    if (d)
        native = d->native;
    else {
        bool writable = false;
        native = nativeOpen(native_path, LINUX_O_RDWR, 0, &writable, &error);
        if (native < 0) {
            linuxSyncUnlock();
            return error ? error : LINUX_EIO;
        }
        temporary = true;
    }
    int64_t done = 0;
    while (!error && (uint64_t)done < count) {
        nativeSeek(native, offset + done, 0, &error);
        if (error) break;
        int64_t written = nativeWrite(native, (const unsigned char *)data + done, count - (size_t)done, &error);
        if (!error && written <= 0) error = LINUX_EIO;
        if (!error) done += written;
    }
    if (temporary) nativeClose(native);
    linuxSyncUnlock();
    return error;
}
bool linuxFilesIsOpen(int fd) {
    linuxSyncLock();
    bool open = get(fd) != NULL;
    linuxSyncUnlock();
    return open;
}
unsigned linuxFilesSocketCount(void) {
    linuxSyncLock();
    unsigned count = 0;
    for (unsigned i = 0; i < LINUX_FILE_MAX_DESCRIPTORS; ++i)
        if (descriptors[i].active && descriptors[i].virtual_kind == VIRTUAL_SOCKET) ++count;
    linuxSyncUnlock();
    return count;
}
void linuxFilesSocketSetNonblocking(int fd, bool enable) {
    linuxSyncLock();
    Descriptor *d = get(fd);
    if (d && d->virtual_kind == VIRTUAL_SOCKET) d->flags = enable ? (d->flags | LINUX_O_NONBLOCK) : (d->flags & ~LINUX_O_NONBLOCK);
    linuxSyncUnlock();
}
// fcntl(2) for managed descriptors: close-on-exec and status flags are recorded; locks and duplication are refused.
enum {
    LINUX_F_DUPFD = 0,
    LINUX_F_GETFD = 1,
    LINUX_F_SETFD = 2,
    LINUX_F_GETFL = 3,
    LINUX_F_SETFL = 4,
    LINUX_F_DUPFD_CLOEXEC = 1030,
    LINUX_FD_CLOEXEC = 1,
    LINUX_F_GETLK = 5,
    LINUX_F_SETLK = 6,
    LINUX_F_SETLKW = 7,
    LINUX_F_OFD_GETLK = 36,
    LINUX_F_OFD_SETLK = 37,
    LINUX_F_OFD_SETLKW = 38,
    LINUX_F_UNLCK = 2
};
typedef struct {
    int16_t type, whence;
    int64_t start, length;
    int32_t pid;
    int32_t padding;
} LinuxFlock;  // aarch64 struct flock
_Static_assert(sizeof(LinuxFlock) == 32 && offsetof(LinuxFlock, start) == 8 && offsetof(LinuxFlock, pid) == 24, "aarch64 flock layout");
int linuxAbiFcntl(int fd, int command, ...) {
    intptr_t argument = 0;  // pointer-sized: struct flock pointers travel through the variadic slot
    {
        va_list args;
        va_start(args, command);
        argument = va_arg(args, intptr_t);
        va_end(args);
    }
    linuxSyncLock();
    Descriptor *d = get(fd);
    if (!d) return finish(LINUX_EBADF);
    int result = 0;
    switch (command) {
        case LINUX_F_GETFD: result = (d->flags & LINUX_O_CLOEXEC) ? LINUX_FD_CLOEXEC : 0; break;
        case LINUX_F_SETFD:
            if (argument & LINUX_FD_CLOEXEC)
                d->flags |= LINUX_O_CLOEXEC;
            else
                d->flags &= ~LINUX_O_CLOEXEC;
            break;
        case LINUX_F_GETFL: result = d->flags & (3 | LINUX_O_APPEND | LINUX_O_NONBLOCK); break;
        case LINUX_F_SETFL: {
            int wanted = (int)argument & LINUX_O_NONBLOCK;
            if (d->virtual_kind == VIRTUAL_SOCKET && wanted != (d->flags & LINUX_O_NONBLOCK)) {
                int socket_error = 0;
                if (linuxNetNativeSetNonblocking(d->native, wanted != 0, &socket_error) < 0) return finish(socket_error);
            }
            d->flags = (d->flags & ~(LINUX_O_APPEND | LINUX_O_NONBLOCK)) | ((int)argument & (LINUX_O_APPEND | LINUX_O_NONBLOCK));
            break;
        }
        case LINUX_F_GETLK:
        case LINUX_F_OFD_GETLK: {
            // One process: advisory locks never conflict, so nothing ever blocks a lock request (the JVM tracks its own).
            LinuxFlock *lock = (LinuxFlock *)(uintptr_t)argument;
            if (!lock) return finish(LINUX_EFAULT);
            lock->type = LINUX_F_UNLCK;
            break;
        }
        case LINUX_F_SETLK:
        case LINUX_F_SETLKW:
        case LINUX_F_OFD_SETLK:
        case LINUX_F_OFD_SETLKW:
            if (!argument) return finish(LINUX_EFAULT);
            break;
        case LINUX_F_DUPFD:
        case LINUX_F_DUPFD_CLOEXEC: return finish(LINUX_ENOSYS);
        default: return finish(LINUX_EINVAL);
    }
    linuxSyncUnlock();
    return result;
}
int linuxAbiUnlink(const char *name) {
    linuxSyncLock();
    char native_path[LINUX_FILE_PATH_MAX];
    int error = path(name, native_path);
    if (!error) {

        Descriptor *open_file = byPath(native_path);
        if (open_file && !open_file->virtual_kind) {
            // Unlink while open: keep the name until the last descriptor closes (the file must exist now).
            LinuxFileStat value;
            error = statNative(native_path, false, &value);
            if (!error)
                for (unsigned i = 0; i < LINUX_FILE_MAX_DESCRIPTORS; ++i)
                    if (descriptors[i].active && !strcmp(descriptors[i].path, native_path)) descriptors[i].delete_on_close = true;
        } else {
            error = nativeUnlink(native_path);
            if (!error) forgetPath(native_path);
            if (!error)
                for (unsigned i = 0; i < LINUX_FILE_MAX_DESCRIPTORS; ++i)
                    if (descriptors[i].active && !strcmp(descriptors[i].path, native_path)) descriptors[i].path[0] = 0;
        }
    }
    return finish(error);
}
int linuxAbiRename(const char *old_name, const char *new_name) {
    linuxSyncLock();
    char old_path[LINUX_FILE_PATH_MAX], new_path[LINUX_FILE_PATH_MAX];
    int error = path(old_name, old_path);
    if (!error) error = path(new_name, new_path);
    if (!error && strcmp(old_path, new_path) && (cwdAncestor(old_path) || cwdAncestor(new_path))) error = LINUX_EBUSY;
    if (!error) {
        error = nativeRename(old_path, new_path);
        if (!error) renameObserved(old_path, new_path);
        if (!error && strcmp(old_path, new_path))
            for (unsigned i = 0; i < LINUX_FILE_MAX_DESCRIPTORS; ++i)
                if (descriptors[i].active) {
                    if (!strcmp(descriptors[i].path, old_path))
                        strcpy(descriptors[i].path, new_path);
                    else if (!strcmp(descriptors[i].path, new_path))
                        descriptors[i].path[0] = 0;
                }
    }
    return finish(error);
}
bool linuxFilesReset(void) {
    if (linuxStdioHasStreams()) return false;  // reset requires quiescent callers
    linuxSyncLock();
    bool ok = true;
    for (unsigned i = 0; i < LINUX_DIRECTORY_MAX; ++i)
        if (directories[i].active) {
            bool released = false;
            int error = nativeDirClose(directories[i].native, &released);
            if (error) ok = false;
            if (released) { directoryClear(&directories[i]); }
        }
    for (unsigned i = 0; i < LINUX_FILE_MAX_DESCRIPTORS; ++i)
        if (descriptors[i].active) {
            int error = closeRecord(&descriptors[i]);
            if (pending_socket_close >= 0) {
                linuxNetNativeClose(pending_socket_close);
                pending_socket_close = -1;
            }
            if (pending_vfd_object >= 0) {
                linuxVfdRelease(pending_vfd_kind, pending_vfd_object, pending_vfd_access);
                pending_vfd_object = -1;
            }
            if (error) ok = false;
        }
    if (ok) {
        root[0] = cwd[0] = 0;
        memset(observed_paths, 0, sizeof(observed_paths));
        observed_overflow = false;
    }
    linuxSyncUnlock();
    return ok;
}

// Directory operations share the file metadata/I/O lock. No nested ABI calls.
static int dirFinish(int error) { return finish(error); }
static Directory *directory(void *handle) {
    for (unsigned i = 0; i < LINUX_DIRECTORY_MAX; ++i)
        if (handle == &directories[i] && directories[i].active) return &directories[i];
    return NULL;
}
void *linuxAbiOpendir(const char *name) {
    linuxSyncLock();
    char native_path[LINUX_FILE_PATH_MAX];
    int error = path(name, native_path);
    LinuxFileStat value;
    if (!error) error = statNative(native_path, true, &value);
    if (!error && (value.mode & 0170000) != 0040000) error = LINUX_ENOTDIR;
    unsigned index = 0;
    if (!error) {
        while (index < LINUX_DIRECTORY_MAX && directories[index].active) ++index;
        if (index == LINUX_DIRECTORY_MAX) error = LINUX_EMFILE;
    }
    if (!error && observed_overflow) error = LINUX_EOVERFLOW;
    size_t count = 0;
    ObservedEntry *snapshot = NULL;
    if (!error) {
        for (unsigned i = 0; i < LINUX_DIRECTORY_OBSERVED_MAX; ++i)
            if (observedChild(observed_paths[i], native_path)) ++count;
        if (count) {
            snapshot = calloc(count, sizeof(*snapshot));
            if (!snapshot) error = LINUX_ENOMEM;
        }
    }
    if (!error && count) {
        size_t at = 0;
        for (unsigned i = 0; i < LINUX_DIRECTORY_OBSERVED_MAX; ++i) {
            const char *base = observedChild(observed_paths[i], native_path);
            if (base) strcpy(snapshot[at++].name, base);
        }
    }
    void *native = NULL;
    if (!error) native = nativeDirOpen(native_path, &error);
    if (!error && !native) error = LINUX_EIO;
    Directory *result = NULL;
    if (!error) {
        result = &directories[index];
        *result = (Directory){.active = true, .native = native, .observed = snapshot, .observed_count = count};
        strcpy(result->path, native_path);
    } else
        free(snapshot);
    dirFinish(error);
    return result;
}
LinuxDirectoryEntry *linuxAbiReaddir64(void *handle) {
    linuxSyncLock();
    Directory *d = directory(handle);
    int error = d ? 0 : LINUX_EBADF;
    bool end = false;
    LinuxDirectoryEntry value = {0};
    if (!error && d->position == INT64_MAX) error = LINUX_EOVERFLOW;
    if (!error) {

        if (!d->native_end) {
            error = nativeDirRead(d->native, &value, &end);
            if (!error && end) d->native_end = true;
            if (!error && !end)
                for (size_t i = 0; i < d->observed_count; ++i)
                    if (!strcmp(value.name, d->observed[i].name)) d->observed[i].seen = true;
        } else
            end = true;
        while (!error && end && d->observed_next < d->observed_count) {
            ObservedEntry *candidate = &d->observed[d->observed_next];
            if (candidate->seen) {
                ++d->observed_next;
                continue;
            }
            char candidate_path[LINUX_FILE_PATH_MAX];
            size_t parent = strlen(d->path), length = strlen(candidate->name);
            memcpy(candidate_path, d->path, parent);
            candidate_path[parent] = '/';
            memcpy(candidate_path + parent + 1, candidate->name, length + 1);
            error = nativeLookup(candidate_path, &value);
            if (error == LINUX_ENOENT || error == LINUX_ENOTDIR) {
                error = 0;
                ++d->observed_next;
                continue;
            }
            if (error) break;  // Retry this candidate on the next call.
            ++d->observed_next;
            if (length >= sizeof(value.name)) {
                error = LINUX_ENAMETOOLONG;
                break;
            }
            memcpy(value.name, candidate->name, length + 1);
            end = false;
        }
    }
    if (!error && !end) {
        size_t length = 0;
        while (length < sizeof(value.name) && value.name[length]) ++length;
        if (!length || length == sizeof(value.name))
            error = LINUX_EOVERFLOW;
        else {
            value.offset = ++d->position;
            value.record_bytes = (uint16_t)((19 + length + 1 + 7) & ~7u);
            d->entry = value;
        }
    }
    dirFinish(error);
    return error || end ? NULL : &d->entry;
}
int linuxAbiClosedir(void *handle) {
    linuxSyncLock();
    Directory *d = directory(handle);
    bool released = false;
    int error = d ? nativeDirClose(d->native, &released) : LINUX_EBADF;
    if (released) { directoryClear(d); }
    return dirFinish(error);
}
int linuxAbiMkdir(const char *name, unsigned mode) {
    linuxSyncLock();
    char native_path[LINUX_FILE_PATH_MAX];
    int error = pathEx(name, native_path, false);
    if (!error) {
        error = nativeMkdir(native_path, mode & 0777);
        if (!error) observePath(native_path);
    }
    return dirFinish(error);
}
int linuxAbiRmdir(const char *name) {
    linuxSyncLock();
    char native_path[LINUX_FILE_PATH_MAX];
    int error = path(name, native_path);
    if (!error && cwdAncestor(native_path)) error = LINUX_EBUSY;
    LinuxFileStat value;
    if (!error) error = statNative(native_path, true, &value);
    if (!error && (value.mode & 0170000) != 0040000) error = LINUX_ENOTDIR;
    if (!error) {
        error = nativeRmdir(native_path);
        if (!error) forgetPath(native_path);
    }
    return dirFinish(error);
}
int linuxAbiRemove(const char *name) {
    linuxSyncLock();
    char native_path[LINUX_FILE_PATH_MAX];
    int error = path(name, native_path);
    LinuxFileStat value;
    if (!error) error = statNative(native_path, false, &value);
    if (!error) {

        if ((value.mode & 0170000) == 0040000)
            error = cwdAncestor(native_path) ? LINUX_EBUSY : nativeRmdir(native_path);
        else {
            error = nativeUnlink(native_path);
            if (!error)
                for (unsigned i = 0; i < LINUX_FILE_MAX_DESCRIPTORS; ++i)
                    if (descriptors[i].active && !strcmp(descriptors[i].path, native_path)) descriptors[i].path[0] = 0;
        }
    }
    if (!error) forgetPath(native_path);
    return dirFinish(error);
}
int linuxAbiChdir(const char *name) {
    linuxSyncLock();
    char native_path[LINUX_FILE_PATH_MAX];
    int error = path(name, native_path);
    LinuxFileStat value;
    if (!error) error = statNative(native_path, true, &value);
    if (!error && (value.mode & 0170000) != 0040000) error = LINUX_ENOTDIR;
    if (!error) {
        const char *relative = relativeToRoot(native_path);
        if (relative)
            strcpy(cwd, relative);
        else
            error = LINUX_ENOENT;
    }
    return dirFinish(error);
}
char *linuxAbiGetcwd(char *buffer, size_t bytes) {
    linuxSyncLock();
    int error = *root ? 0 : LINUX_ENOSYS;
    const char *text = *cwd ? cwd : "/";
    size_t length = strlen(text) + 1;
    if (!error && buffer && !bytes) error = LINUX_EINVAL;
    if (!error && bytes && bytes < length) error = LINUX_ERANGE;
    LinuxFileStat value;
    if (!error) {
        char native_path[LINUX_FILE_PATH_MAX];
        error = path(".", native_path);
        if (!error) error = statNative(native_path, true, &value);
    }
    if (!error && (value.mode & 0170000) != 0040000) error = LINUX_ENOTDIR;
    char *result = buffer;
    if (!error && !result) {
        result = linuxAbiMalloc(bytes ? bytes : length);
        if (!result) error = LINUX_ENOMEM;
    }
    if (!error) { memcpy(result, text, length); }
    dirFinish(error);
    return error ? NULL : result;
}
