#pragma once
#include "linux_abi.h"
#include <stdio.h>
#define LINUX_FILE_MAX_DESCRIPTORS 128u
#define LINUX_FILE_PATH_MAX 512u
enum {
    LINUX_O_RDONLY = 0,
    LINUX_O_WRONLY = 1,
    LINUX_O_RDWR = 2,
    LINUX_O_CREAT = 0x40,
    LINUX_O_EXCL = 0x80,
    LINUX_O_NOCTTY = 0x100,
    LINUX_O_TRUNC = 0x200,
    LINUX_O_APPEND = 0x400,
    LINUX_O_NONBLOCK = 0x800,
    LINUX_O_CLOEXEC = 0x80000
};
typedef struct {
    uint64_t device, inode;
    uint32_t mode, links, uid, gid;
    uint64_t rdevice, pad1;
    int64_t size;
    int32_t block_size, pad2;
    int64_t blocks;
    LinuxTimespec access_time, modify_time, change_time;
    uint32_t reserved[2];
} LinuxFileStat;
// Only change root with no descriptors open. Absolute and relative Linux paths
// resolve under this native directory; no process cwd or device path passthrough.
bool linuxFilesSetRoot(const char *root);
bool linuxFilesReset(void);
int linuxAbiOpen(const char *path, int flags, ...);
int linuxAbiClose(int fd);
int64_t linuxAbiRead(int fd, void *buffer, size_t count);
int64_t linuxAbiWrite(int fd, const void *buffer, size_t count);
int64_t linuxAbiLseek(int fd, int64_t offset, int whence);
int64_t linuxAbiPread(int fd, void *buffer, size_t count, int64_t offset);
int64_t linuxAbiPwrite(int fd, const void *buffer, size_t count, int64_t offset);
int linuxAbiFsync(int fd);
int linuxAbiFtruncate(int fd, int64_t size);
int linuxAbiXstat(int version, const char *path, LinuxFileStat *output);
int linuxAbiLxstat(int version, const char *path, LinuxFileStat *output);
int linuxAbiFxstat(int version, int fd, LinuxFileStat *output);
int linuxAbiAccess(const char *path, int mode);
int linuxFilesNativePath(const char *path, char output[LINUX_FILE_PATH_MAX]);  // guest path -> SD path (0 or a Linux errno as -1 result)
int linuxAbiFcntl(int fd, int command, ...);
// Sockets (linux_net.c) live in the same descriptor table: a socket descriptor wraps a native handle of the network layer.
int linuxFilesSocketCreate(int handle, int flags);             // the new guest descriptor, or -1 with errno
bool linuxFilesSocketLookup(int fd, int *handle, int *flags);  // false with errno EBADF / ENOTSOCK
bool linuxFilesSocketPeek(int fd, int *handle, int *flags);    // the same, without touching errno
bool linuxFilesIsOpen(int fd);
// Eventfd, pipe and epoll descriptors (linux_vfd.c): `object` is the index of the in-memory object.
int linuxFilesVirtualCreate(int kind, int object, int flags);
bool linuxFilesVirtualPeek(int fd, int *kind, int *object, int *flags);
// For shared writable file mappings (linux_vm.c): the native path and flags of a regular-file descriptor, and a write that
// reaches the file through the open descriptor of that path (Horizon allows one writable handle) or a temporary one.
bool linuxFilesDescriptorInfo(int fd, char path[LINUX_FILE_PATH_MAX], int *flags);                 // false with errno EBADF / ENODEV
int linuxFilesWriteBack(const char *native_path, int64_t offset, const void *data, size_t count);  // 0 or a Linux errno
unsigned linuxFilesSocketCount(void);
void linuxFilesSocketSetNonblocking(int fd, bool enable);
int linuxAbiUnlink(const char *path);
int linuxAbiRename(const char *old_path, const char *new_path);
// Internal stream bridge: generation checks occur under the I/O lock, so a
// stale FILE token cannot read/write/close a newly reused descriptor number.
int linuxFileStreamAttach(int, bool readable, bool writable, bool append, uint64_t *generation);
int linuxFileStreamClose(int, uint64_t);
int64_t linuxFileStreamRead(int, uint64_t, void *, size_t);
int64_t linuxFileStreamWrite(int, uint64_t, const void *, size_t);
int64_t linuxFileStreamSeek(int, uint64_t, int64_t, int);
