#include "linux_process.h"
#include <limits.h>
#include <string.h>
#include <switch.h>

_Static_assert(sizeof(size_t) == 8 && sizeof(LinuxTimespec) == 16, "Linux LP64 process ABI");
static int fail(int error) {
    *linuxAbiErrnoLocation() = error;
    return -1;
}

// What the process knows about itself, from the kernel. Each returns a Linux errno, zero on success.
static int nativePid(uint64_t *pid) { return R_SUCCEEDED(svcGetProcessId(pid, CUR_PROCESS_HANDLE)) ? 0 : LINUX_EIO; }
static int nativeAffinity(bool main_thread, uint64_t *mask) {
    s32 preferred;
    Handle thread = main_thread ? envGetMainThreadHandle() : CUR_THREAD_HANDLE;
    return R_SUCCEEDED(svcGetThreadCoreMask(&preferred, mask, thread)) ? 0 : LINUX_EIO;
}
static int nativeCores(uint64_t *mask) { return R_SUCCEEDED(svcGetInfo(mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) ? 0 : LINUX_EIO; }
static int nativeMemory(uint64_t *total, uint64_t *used) {
    Result rc = svcGetInfo(total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    if (R_SUCCEEDED(rc)) rc = svcGetInfo(used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    return R_SUCCEEDED(rc) ? 0 : LINUX_EIO;
}
int linuxProcessGetpid(void) {
    uint64_t pid;
    int error = nativePid(&pid);
    if (error) return fail(error);
    if (!pid || pid > INT_MAX) return fail(LINUX_EOVERFLOW);
    return (int)pid;
}
void *linuxProcessCpuAlloc(size_t count) {
    if (count > LINUX_CPU_SET_MAX_BYTES * 8u) {
        fail(LINUX_ENOMEM);
        return NULL;
    }
    // malloc semantics: the caller initializes its mask, including padding.
    return linuxAbiMalloc(((count + 63) / 64) * 8);
}
void linuxProcessCpuFree(void *set) { linuxAbiFree(set); }
int linuxProcessCpuCount(size_t bytes, const void *set) {
    if (bytes > LINUX_CPU_SET_MAX_BYTES) return fail(LINUX_EINVAL);
    size_t words = bytes / 8;
    if (words && !set) return fail(LINUX_EFAULT);
    int count = 0;
    for (size_t i = 0; i < words; ++i) {
        uint64_t word;
        memcpy(&word, (const unsigned char *)set + i * 8, 8);
        count += __builtin_popcountll(word);
    }
    // glibc ignores any trailing incomplete word.
    return count;
}
int linuxProcessGetAffinity(int pid, size_t bytes, void *set) {
    if (pid < 0) return fail(LINUX_ESRCH);
    if (!set) return fail(LINUX_EFAULT);
    if (bytes < 8 || bytes % 8 || bytes > LINUX_CPU_SET_MAX_BYTES) return fail(LINUX_EINVAL);
    if (pid) {
        int own = linuxProcessGetpid();
        if (own < 0) return -1;
        if (own != pid) return fail(LINUX_ESRCH);
    }
    uint64_t mask;
    int error = nativeAffinity(pid != 0, &mask);
    if (error) return fail(error);
    if (!mask) return fail(LINUX_EIO);
    memset(set, 0, bytes);
    memcpy(set, &mask, 8);
    return 0;
}
int64_t linuxProcessSysconf(int name) {
    if (name == 83 || name == 84) {
        uint64_t mask;
        int error = nativeCores(&mask);
        if (error) return fail(error);
        if (!mask) return fail(LINUX_EIO);
        return __builtin_popcountll(mask);
    }
    if (name == 85 || name == 86) {
        uint64_t total, used;
        int error = nativeMemory(&total, &used);
        if (error) return fail(error);
        if (used > total || !total) return fail(LINUX_EIO);
        return (int64_t)((name == 85 ? total : total - used) / 4096);
    }
    return fail(name < 0 ? LINUX_EINVAL : LINUX_ENOSYS);
}
static int monotonic(int64_t *value) {
    LinuxTimespec time;
    int error = linuxAbiReadClock(1, &time);
    if (error) return error;
    if (time.seconds < 0 || time.nanoseconds < 0 || time.nanoseconds >= 1000000000 || time.seconds > (INT64_MAX - time.nanoseconds) / 1000000000)
        return LINUX_EOVERFLOW;
    *value = time.seconds * 1000000000 + time.nanoseconds;
    return 0;
}
// The sleep is never interrupted, so `remaining` is never written.
int linuxProcessNanosleep(const LinuxTimespec *request, LinuxTimespec *remaining) {
    (void)remaining;
    if (!request) return fail(LINUX_EFAULT);
    LinuxTimespec duration = *request;  // request and remaining may alias.
    if (duration.seconds < 0 || duration.nanoseconds < 0 || duration.nanoseconds >= 1000000000) return fail(LINUX_EINVAL);
    if (duration.seconds > (INT64_MAX - duration.nanoseconds) / 1000000000) return fail(LINUX_EOVERFLOW);
    int64_t ns = duration.seconds * 1000000000 + duration.nanoseconds;
    if (!ns) return 0;
    int64_t start;
    int error = monotonic(&start);
    if (error) return fail(error);
    if (ns > INT64_MAX - start) return fail(LINUX_EOVERFLOW);
    int64_t deadline = start + ns, left = ns;
    for (;;) {
        svcSleepThread(left);
        int64_t now;
        int clock_error = monotonic(&now);
        if (clock_error) return fail(clock_error);
        if (now < start) return fail(LINUX_EIO);
        left = now < deadline ? deadline - now : 0;
        if (!left) return 0;
        // Horizon may wake a thread early; enforce the monotonic deadline.
    }
}
