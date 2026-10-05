#pragma once
#include "elf_imports.h"
#include <stddef.h>

typedef struct {
    int64_t seconds, nanoseconds;
} LinuxTimespec;
typedef struct {
    int64_t seconds, microseconds;
} LinuxTimeval;

enum {
    LINUX_EPERM = 1,
    LINUX_ENOENT = 2,
    LINUX_ESRCH = 3,
    LINUX_EINTR = 4,
    LINUX_EIO = 5,
    LINUX_EBADF = 9,
    LINUX_EAGAIN = 11,
    LINUX_ENOMEM = 12,
    LINUX_EACCES = 13,
    LINUX_EFAULT = 14,
    LINUX_EBUSY = 16,
    LINUX_EEXIST = 17,
    LINUX_ENOTDIR = 20,
    LINUX_EISDIR = 21,
    LINUX_EINVAL = 22,
    LINUX_ENFILE = 23,
    LINUX_EMFILE = 24,
    LINUX_EFBIG = 27,
    LINUX_ENOSPC = 28,
    LINUX_ESPIPE = 29,
    LINUX_EROFS = 30,
    LINUX_ERANGE = 34,
    LINUX_EDEADLK = 35,
    LINUX_ENAMETOOLONG = 36,
    LINUX_ENOSYS = 38,
    LINUX_ENOTEMPTY = 39,
    LINUX_ELOOP = 40,
    LINUX_EOVERFLOW = 75,
    LINUX_EILSEQ = 84,
    LINUX_ENOTSUP = 95,
    LINUX_ETIMEDOUT = 110
};

bool linuxAbiResolve(void *context, const ElfImport *symbol, uintptr_t *address);
// Name lookup for dlsym: any registered function or object, whatever its version or provider.
bool linuxAbiLookup(const char *name, uintptr_t *address);
int *linuxAbiErrnoLocation(void);
void *linuxAbiMalloc(size_t bytes);
void *linuxAbiCalloc(size_t count, size_t bytes);
void *linuxAbiRealloc(void *memory, size_t bytes);
void linuxAbiFree(void *memory);
int64_t linuxAbiStrtol(const char *text, char **end, int base);
int64_t linuxAbiSysconf(int name);
// clock 0 is the wall clock, 1 the monotonic one. Returns a Linux errno value on failure, zero on success.
int linuxAbiReadClock(int clock, LinuxTimespec *time);
