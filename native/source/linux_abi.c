#include "linux_abi.h"
#include "linux_vm.h"
#include "linux_threads.h"
#include "linux_sync.h"
#include "linux_semaphore.h"
#include "linux_files.h"
#include "linux_directories.h"
#include "linux_stdio.h"
#include "linux_format.h"
#include "linux_process.h"
#include "linux_runtime.h"
#include "linux_libc_extra.h"
#include "linux_net.h"
#include "linux_vfd.h"
#include <math.h>
#include <setjmp.h>
#include <wchar.h>
#include <elf.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <switch.h>

_Static_assert(sizeof(uintptr_t) == 8 && sizeof(size_t) == 8, "64-bit ABI required");
_Static_assert(sizeof(LinuxTimespec) == 16 && sizeof(LinuxTimeval) == 16, "Linux LP64 time layout");
static _Thread_local int linux_errno;
int *linuxAbiErrnoLocation(void) { return &linux_errno; }

void *linuxAbiMalloc(size_t bytes) {
    if (bytes > PTRDIFF_MAX) {
        linux_errno = LINUX_ENOMEM;
        return NULL;
    }
    void *memory = malloc(bytes ? bytes : 1);
    if (!memory) linux_errno = LINUX_ENOMEM;
    return memory;
}
void *linuxAbiCalloc(size_t count, size_t bytes) {
    if (bytes && count > (size_t)PTRDIFF_MAX / bytes) {
        linux_errno = LINUX_ENOMEM;
        return NULL;
    }
    size_t total = count * bytes;
    void *memory = calloc(1, total ? total : 1);
    if (!memory) linux_errno = LINUX_ENOMEM;
    return memory;
}
void *linuxAbiRealloc(void *memory, size_t bytes) {
    if (bytes > PTRDIFF_MAX) {
        linux_errno = LINUX_ENOMEM;
        return NULL;
    }
    // Match glibc realloc(p,0), including errno preservation.
    if (memory && !bytes) {
        free(memory);
        return NULL;
    }
    void *result = realloc(memory, bytes ? bytes : 1);
    if (!result) linux_errno = LINUX_ENOMEM;
    return result;
}
void linuxAbiFree(void *memory) { free(memory); }
static char *linuxAbiStrdup(const char *text) {
    size_t bytes = strlen(text) + 1;
    char *copy = linuxAbiMalloc(bytes);
    if (copy) memcpy(copy, text, bytes);
    return copy;
}
static bool validBase(int base) { return !base || (base >= 2 && base <= 36); }
int64_t linuxAbiStrtol(const char *text, char **end, int base) {
    if (!validBase(base)) {
        linux_errno = LINUX_EINVAL;
        return 0;
    }
    int saved = errno;
    errno = 0;
    int64_t value = strtoll(text, end, base);
    if (errno == ERANGE) linux_errno = LINUX_ERANGE;
    errno = saved;
    return value;
}
static uint64_t linuxAbiStrtoull(const char *text, char **end, int base) {
    if (!validBase(base)) {
        linux_errno = LINUX_EINVAL;
        return 0;
    }
    int saved = errno;
    errno = 0;
    uint64_t value = strtoull(text, end, base);
    if (errno == ERANGE) linux_errno = LINUX_ERANGE;
    errno = saved;
    return value;
}
static size_t boundedLength(const char *text, size_t bound) {
    size_t length = 0;
    while (length < bound && text[length]) ++length;
    return length;
}
static char *copyEnd(char *target, const char *text) {
    size_t length = strlen(text);
    memcpy(target, text, length + 1);
    return target + length;
}
static char *splitString(char **cursor, const char *delimiters) {
    if (!*cursor) return NULL;
    char *first = *cursor;
    size_t length = strcspn(first, delimiters);
    *cursor = first[length] ? first + length + 1 : NULL;
    first[length] = 0;
    return first;
}
static int isDigit(int c) { return c >= '0' && c <= '9'; }
static int isSpace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static int pageSize(void) { return 4096; }
// Descriptors 0-2 belong to the console: output goes to the console sink, input is at end of file.
static int64_t clientWrite(int fd, const void *buffer, size_t count) {
    return fd == 1 || fd == 2 ? linuxStdioWriteConsoleFd(fd, buffer, count) : linuxAbiWrite(fd, buffer, count);
}
static int64_t clientRead(int fd, void *buffer, size_t count) { return fd == 0 ? 0 : linuxAbiRead(fd, buffer, count); }
int64_t linuxAbiSysconf(int name) {
    // _SC_PAGESIZE is 30 in the client's glibc ABI, not newlib's enum.
    if (name == 30) return 4096;
    return linuxProcessSysconf(name);
}
int linuxAbiReadClock(int clock, LinuxTimespec *time) {
    if (clock == 1) {
        // The newlib clock of libnx derives both clocks from wall time: the architectural counter gives an independent monotonic clock.
        uint64_t ticks = armGetSystemTick(), frequency = armGetSystemTickFreq();
        if (!frequency) return LINUX_EIO;
        time->seconds = ticks / frequency;
        time->nanoseconds = (uint64_t)(((__uint128_t)(ticks % frequency) * 1000000000) / frequency);
        return 0;
    }
    struct timespec native;
    if (clock_gettime(CLOCK_REALTIME, &native)) return errno == EINVAL ? LINUX_EINVAL : LINUX_EIO;
    *time = (LinuxTimespec){native.tv_sec, native.tv_nsec};
    return 0;
}
static int linuxAbiClockGettime(int clock, LinuxTimespec *time) {
    if (!time) {
        linux_errno = LINUX_EFAULT;
        return -1;
    }
    if (clock != 0 && clock != 1) {
        linux_errno = LINUX_EINVAL;
        return -1;
    }
    LinuxTimespec value;
    int error = linuxAbiReadClock(clock, &value);
    if (error) {
        linux_errno = error;
        return -1;
    }
    *time = value;
    return 0;
}
static int linuxAbiGettimeofday(LinuxTimeval *time, void *timezone) {
    // A timezone request requires an adapter; do not invent one.
    if (timezone) {
        linux_errno = LINUX_ENOSYS;
        return -1;
    }
    if (!time) return 0;
    LinuxTimespec value;
    if (linuxAbiClockGettime(0, &value)) return -1;
    *time = (LinuxTimeval){value.seconds, value.nanoseconds / 1000};
    return 0;
}
static int64_t linuxAbiTime(int64_t *result) {
    LinuxTimespec value;
    if (linuxAbiClockGettime(0, &value)) return -1;
    if (result) *result = value.seconds;
    return value.seconds;
}

#include "linux_zlib.h"
typedef struct {
    const char *name;
    uintptr_t address;
    const char *version, *library;
} Binding;
#define BIND(name, function) {name, (uintptr_t)(function), "GLIBC_2.17", "libc.so.6"}
#define BINDV(name, function, version) {name, (uintptr_t)(function), version, "libc.so.6"}
#define BINDZ(name, function, version) {name, (uintptr_t)(function), version, "libz.so.1"}
static const Binding bindings[] = {
    BIND("memcpy", memcpy), BIND("memmove", memmove), BIND("memset", memset), BIND("memcmp", memcmp), BIND("strlen", strlen), BIND("strnlen", boundedLength),
    BIND("strcmp", strcmp), BIND("strncmp", strncmp), BIND("strchr", strchr), BIND("strrchr", strrchr), BIND("strstr", strstr), BIND("strcpy", strcpy),
    BIND("strncpy", strncpy), BIND("strcat", strcat), BIND("strncat", strncat), BIND("strcspn", strcspn), BIND("stpcpy", copyEnd), BIND("strsep", splitString),
    BIND("strdup", linuxAbiStrdup), BIND("__strdup", linuxAbiStrdup), BIND("malloc", linuxAbiMalloc), BIND("calloc", linuxAbiCalloc),
    BIND("realloc", linuxAbiRealloc), BIND("free", linuxAbiFree), BIND("strtol", linuxAbiStrtol), BIND("strtoull", linuxAbiStrtoull), BIND("isdigit", isDigit),
    BIND("isspace", isSpace), BIND("__errno_location", linuxAbiErrnoLocation), BIND("getpagesize", pageSize), BIND("sysconf", linuxAbiSysconf),
    BIND("clock_gettime", linuxAbiClockGettime), BIND("gettimeofday", linuxAbiGettimeofday), BIND("time", linuxAbiTime), BIND("getpid", linuxProcessGetpid),
    BIND("nanosleep", linuxProcessNanosleep), BIND("sched_getaffinity", linuxProcessGetAffinity), BIND("__sched_cpualloc", linuxProcessCpuAlloc),
    BIND("__sched_cpufree", linuxProcessCpuFree), BIND("__sched_cpucount", linuxProcessCpuCount), BIND("mmap", linuxAbiMmap), BIND("mmap64", linuxAbiMmap),
    BIND("mprotect", linuxAbiMprotect), BIND("munmap", linuxAbiMunmap), BIND("pthread_attr_init", linuxPthreadAttrInit),
    BIND("pthread_attr_destroy", linuxPthreadAttrDestroy), BIND("pthread_attr_setdetachstate", linuxPthreadAttrSetDetachState),
    BIND("pthread_self", linuxPthreadSelf), BIND("sched_yield", linuxSchedYield),
    BINDV("pthread_attr_setstacksize", linuxPthreadAttrSetStackSize, "GLIBC_2.34"), BINDV("pthread_attr_getstack", linuxPthreadAttrGetStack, "GLIBC_2.34"),
    BINDV("pthread_attr_getguardsize", linuxPthreadAttrGetGuardSize, "GLIBC_2.34"), BINDV("pthread_getattr_np", linuxPthreadGetattr, "GLIBC_2.32"),
    BINDV("pthread_create", linuxPthreadCreate, "GLIBC_2.34"), BINDV("pthread_join", linuxPthreadJoin, "GLIBC_2.34"),
    BINDV("pthread_key_create", linuxPthreadKeyCreate, "GLIBC_2.34"), BINDV("pthread_key_delete", linuxPthreadKeyDelete, "GLIBC_2.34"),
    BINDV("pthread_getspecific", linuxPthreadGetSpecific, "GLIBC_2.34"), BINDV("pthread_setspecific", linuxPthreadSetSpecific, "GLIBC_2.34"),
    BIND("pthread_mutex_init", linuxPthreadMutexInit), BIND("pthread_mutex_destroy", linuxPthreadMutexDestroy),
    BIND("pthread_mutex_lock", linuxPthreadMutexLock), BIND("pthread_mutex_unlock", linuxPthreadMutexUnlock),
    BINDV("pthread_mutex_trylock", linuxPthreadMutexTryLock, "GLIBC_2.34"), BIND("pthread_condattr_init", linuxPthreadCondAttrInit),
    BIND("pthread_condattr_destroy", linuxPthreadCondAttrDestroy), BINDV("pthread_condattr_setclock", linuxPthreadCondAttrSetClock, "GLIBC_2.34"),
    BIND("pthread_cond_init", linuxPthreadCondInit), BIND("pthread_cond_destroy", linuxPthreadCondDestroy), BIND("pthread_cond_wait", linuxPthreadCondWait),
    BIND("pthread_cond_timedwait", linuxPthreadCondTimedWait), BIND("pthread_cond_signal", linuxPthreadCondSignal),
    BIND("pthread_cond_broadcast", linuxPthreadCondBroadcast), BINDV("sem_init", linuxSemInit, "GLIBC_2.34"),
    BINDV("sem_destroy", linuxSemDestroy, "GLIBC_2.34"), BINDV("sem_wait", linuxSemWait, "GLIBC_2.34"), BINDV("sem_post", linuxSemPost, "GLIBC_2.34"),
    BIND("open", linuxAbiOpen), BIND("open64", linuxAbiOpen), BIND("close", linuxAbiClose), BIND("access", linuxAbiAccess), BIND("fcntl", linuxAbiFcntl),
    BIND("read", clientRead), BIND("write", clientWrite), BIND("lseek", linuxAbiLseek), BIND("lseek64", linuxAbiLseek), BIND("pread64", linuxAbiPread),
    BIND("pwrite64", linuxAbiPwrite), BIND("fsync", linuxAbiFsync), BIND("fdatasync", linuxAbiFsync), BIND("ftruncate64", linuxAbiFtruncate),
    BIND("__xstat", linuxAbiXstat), BIND("__xstat64", linuxAbiXstat), BIND("__lxstat64", linuxAbiLxstat), BIND("__fxstat64", linuxAbiFxstat),
    BIND("unlink", linuxAbiUnlink), BIND("rename", linuxAbiRename), BIND("opendir", linuxAbiOpendir), BIND("readdir64", linuxAbiReaddir64),
    BIND("closedir", linuxAbiClosedir), BIND("mkdir", linuxAbiMkdir), BIND("rmdir", linuxAbiRmdir), BIND("remove", linuxAbiRemove),
    BIND("chdir", linuxAbiChdir), BIND("getcwd", linuxAbiGetcwd), BIND("fopen", linuxStdioFopen), BIND("fopen64", linuxStdioFopen),
    BIND("fdopen", linuxStdioFdopen), BIND("fclose", linuxStdioFclose), BIND("fread", linuxStdioFread), BIND("fwrite", linuxStdioFwrite),
    BIND("fflush", linuxStdioFflush), BIND("feof", linuxStdioFeof), BIND("ferror", linuxStdioFerror), BIND("fgets", linuxStdioFgets),
    BIND("fputs", linuxStdioFputs), BIND("clearerr", linuxStdioClearerr), BIND("fileno", linuxStdioFileno), BIND("fseeko64", linuxStdioFseeko),
    BIND("ftello64", linuxStdioFtello), BIND("snprintf", linuxFormatSnprintf), BIND("vsnprintf", linuxFormatVsnprintf), BIND("fprintf", linuxFormatFprintf),
    BIND("vfprintf", linuxFormatVfprintf), BINDZ("deflateInit2_", linuxZlibDeflateInit2, NULL), BINDZ("inflateInit2_", linuxZlibInflateInit2, NULL),
    BINDZ("deflate", linuxZlibDeflate, NULL), BINDZ("inflate", linuxZlibInflate, NULL), BINDZ("deflateEnd", linuxZlibDeflateEnd, NULL),
    BINDZ("inflateEnd", linuxZlibInflateEnd, NULL), BINDZ("deflateReset", linuxZlibDeflateReset, NULL), BINDZ("inflateReset", linuxZlibInflateReset, NULL),
    BINDZ("deflateParams", linuxZlibDeflateParams, NULL), BINDZ("deflateBound", linuxZlibDeflateBound, "ZLIB_1.2.0"),
    BINDZ("deflateSetDictionary", linuxZlibDeflateSetDictionary, NULL), BINDZ("inflateSetDictionary", linuxZlibInflateSetDictionary, NULL),
    BINDZ("deflateSetHeader", linuxZlibDeflateSetHeader, "ZLIB_1.2.2"),
    // Process runtime: environment, identity, limits, recorded signals, intercepted termination, lookup facade.
    BIND("getauxval", linuxRuntimeGetauxval), BINDV("dlclose", linuxRuntimeDlclose, "GLIBC_2.34"), BINDV("dlerror", linuxRuntimeDlerror, "GLIBC_2.34"),
    BIND("dl_iterate_phdr", linuxRuntimeDlIteratePhdr), BINDV("dladdr", linuxRuntimeDladdr, "GLIBC_2.34"), BIND("readlink", linuxRuntimeReadlink),
    BIND("__libc_current_sigrtmax", linuxRuntimeSigrtmax), BIND("syscall", linuxRuntimeSyscall), BIND("setlocale", linuxRuntimeSetlocale),
    BIND("getenv", linuxRuntimeGetenv), BIND("getuid", linuxRuntimeGetuid), BIND("getpwuid_r", linuxRuntimeGetpwuidR), BIND("uname", linuxRuntimeUname),
    BIND("getrlimit", linuxRuntimeGetrlimit), BIND("getrlimit64", linuxRuntimeGetrlimit), BIND("setrlimit", linuxRuntimeSetrlimit),
    BIND("strerror_r", linuxRuntimeStrerrorR), BIND("__xpg_strerror_r", linuxRuntimeXpgStrerrorR), BIND("realpath", linuxRuntimeRealpath),
    BINDV("pthread_setname_np", linuxRuntimeSetThreadName, "GLIBC_2.34"), BIND("sigemptyset", linuxRuntimeSigemptyset),
    BIND("sigaddset", linuxRuntimeSigaddset), BIND("sigprocmask", linuxRuntimeSigprocmask), BIND("sigaction", linuxRuntimeSigaction),
    BIND("exit", linuxRuntimeExit), BIND("_exit", linuxRuntimeExitImmediate), BIND("abort", linuxRuntimeAbort),
    BIND("__stack_chk_fail", linuxRuntimeStackCheckFail), BIND("mremap", linuxRuntimeMremap), BINDV("dlopen", linuxRuntimeDlopen, "GLIBC_2.34"),
    BINDV("dlsym", linuxRuntimeDlsym, "GLIBC_2.34"),
    // ---- run-time libraries (LWJGL, libgdx, FreeType, the C++ runtime): libm, fortify, locale, wide characters, time, threads
    BIND("sin", sin), BIND("cos", cos), BIND("cosf", cosf), BIND("exp", exp), BIND("fmod", fmod), BIND("frexp", frexp), BIND("frexpl", frexpl),
    BIND("ldexp", ldexp), BIND("log", log), BIND("pow", pow), BIND("powf", powf), BIND("sqrt", sqrt), BIND("sqrtf", sqrtf), BIND("acos", acos),
    BIND("sincos", linuxExtraSincos), BIND("__snprintf_chk", linuxExtraSnprintfChk), BIND("__vsnprintf_chk", linuxExtraVsnprintfChk),
    BIND("sprintf", linuxExtraSprintf), BIND("__sprintf_chk", linuxExtraSprintfChk), BIND("__fprintf_chk", linuxExtraFprintfChk),
    BIND("printf", linuxExtraPrintf), BIND("__printf_chk", linuxExtraPrintfChk), BIND("puts", linuxExtraPuts), BIND("__memcpy_chk", linuxExtraMemcpyChk),
    BIND("__memmove_chk", linuxExtraMemmoveChk), BIND("__memset_chk", linuxExtraMemsetChk), BIND("__strcpy_chk", linuxExtraStrcpyChk),
    BIND("__strcat_chk", linuxExtraStrcatChk), BIND("__assert_fail", linuxExtraAssertFail), BIND("__isoc99_sscanf", sscanf), BIND("__isoc99_vsscanf", vsscanf),
    BIND("__isoc99_fscanf", linuxExtraFscanf), BIND("fgetc", linuxExtraFgetc), BIND("getc", linuxExtraFgetc), BIND("fputc", linuxExtraFputc),
    BIND("putc", linuxExtraFputc), BIND("ungetc", linuxExtraUngetc), BIND("setvbuf", linuxExtraSetvbuf), BIND("fseek", linuxExtraFseek),
    BIND("ftell", linuxExtraFtell), BIND("fseeko", linuxStdioFseeko), BIND("ftello", linuxStdioFtello), BIND("__getdelim", linuxExtraGetdelim),
    BIND("getdelim", linuxExtraGetdelim), BIND("getline", linuxExtraGetline), BIND("getwc", linuxExtraWideEof), BIND("putwc", linuxExtraWideEof),
    BIND("ungetwc", linuxExtraWideEof), BIND("__newlocale", linuxExtraNewlocale), BIND("newlocale", linuxExtraNewlocale),
    BIND("__duplocale", linuxExtraDuplocale), BIND("duplocale", linuxExtraDuplocale), BIND("__freelocale", linuxExtraFreelocale),
    BIND("freelocale", linuxExtraFreelocale), BIND("__uselocale", linuxExtraUselocale), BIND("uselocale", linuxExtraUselocale),
    BIND("__ctype_get_mb_cur_max", linuxExtraMbCurMax), BIND("nl_langinfo", linuxExtraNlLanginfo), BIND("__nl_langinfo_l", linuxExtraNlLanginfoL),
    BIND("__strcoll_l", linuxExtraStrcollL), BIND("__strxfrm_l", linuxExtraStrxfrmL), BIND("__wcscoll_l", linuxExtraWcscollL),
    BIND("__wcsxfrm_l", linuxExtraWcsxfrmL), BIND("__strtod_l", linuxExtraStrtodL), BIND("__strtof_l", linuxExtraStrtofL),
    BIND("strtold_l", linuxExtraStrtoldL), BIND("strtold", strtold), BIND("strtoul", linuxAbiStrtoull), BIND("__wctype_l", linuxExtraWctypeL),
    BIND("wctype", linuxExtraWctype), BIND("__iswctype_l", linuxExtraIswctypeL), BIND("iswctype", linuxExtraIswctype),
    BIND("__towlower_l", linuxExtraTowlowerL), BIND("__towupper_l", linuxExtraTowupperL), BIND("tolower", linuxExtraTolower),
    BIND("toupper", linuxExtraToupper), BIND("btowc", linuxExtraBtowc), BIND("wctob", linuxExtraWctob), BIND("mbrtowc", linuxExtraMbrtowc),
    BIND("wcrtomb", linuxExtraWcrtomb), BIND("mbsrtowcs", linuxExtraMbsrtowcs), BIND("mbsnrtowcs", linuxExtraMbsnrtowcs),
    BIND("wcsnrtombs", linuxExtraWcsnrtombs), BIND("wcslen", linuxExtraWcslen), BIND("wcscmp", linuxExtraWcscmp), BIND("wmemchr", linuxExtraWmemchr),
    BIND("wmemcmp", linuxExtraWmemcmp), BIND("wmemcpy", linuxExtraWmemcpy), BIND("wmemmove", linuxExtraWmemmove), BIND("wmemset", linuxExtraWmemset),
    BIND("gmtime_r", linuxExtraGmtimeR), BIND("localtime_r", linuxExtraLocaltimeR), BIND("strftime", linuxExtraStrftime),
    BIND("__strftime_l", linuxExtraStrftimeL), BIND("__wcsftime_l", linuxExtraZero), BIND("timegm", linuxExtraTimegm), BIND("tzset", linuxExtraTzset),
    BIND("pthread_once", linuxExtraPthreadOnce), BIND("pthread_rwlock_init", linuxExtraRwlockInit), BIND("pthread_rwlock_destroy", linuxExtraRwlockDestroy),
    BIND("pthread_rwlock_rdlock", linuxExtraRwlockRdlock), BIND("pthread_rwlock_tryrdlock", linuxExtraRwlockTryRdlock),
    BIND("pthread_rwlock_wrlock", linuxExtraRwlockWrlock), BIND("pthread_rwlock_trywrlock", linuxExtraRwlockTryWrlock),
    BIND("pthread_rwlock_unlock", linuxExtraRwlockUnlock), BIND("pthread_mutexattr_init", linuxExtraMutexattrInit),
    BIND("pthread_mutexattr_destroy", linuxExtraMutexattrDestroy), BIND("pthread_mutexattr_settype", linuxExtraMutexattrSettype),
    BIND("pthread_sigmask", linuxExtraPthreadSigmask), BIND("pthread_getname_np", linuxExtraPthreadGetnameNp), BIND("pthread_detach", linuxPthreadDetach),
    BIND("sched_getscheduler", linuxExtraSchedGetscheduler), BIND("sched_getparam", linuxExtraSchedGetparam),
    BIND("sched_setscheduler", linuxExtraSchedSetscheduler), BIND("setpriority", linuxExtraSetpriority), BIND("sched_getcpu", linuxExtraSchedGetcpu),
    BIND("sched_setaffinity", linuxExtraSchedSetaffinity), BIND("get_nprocs", linuxExtraGetNprocs), BIND("geteuid", linuxExtraGetid),
    BIND("getgid", linuxExtraGetid), BIND("getegid", linuxExtraGetid), BIND("getppid", linuxExtraGetppid), BIND("umask", linuxExtraUmask),
    BIND("getentropy", linuxExtraGetentropy), BIND("getrandom", linuxExtraGetrandom), BIND("arc4random", linuxExtraArc4random),
    BIND("secure_getenv", linuxExtraSecureGetenv), BIND("__cxa_atexit", linuxExtraCxaAtexit), BIND("atexit", linuxExtraZero),
    BIND("__cxa_thread_atexit_impl", linuxExtraCxaThreadAtexit), BIND("__register_atfork", linuxExtraRegisterAtfork),
    BIND("_dl_find_object", linuxExtraDlFindObject), BIND("aligned_alloc", linuxExtraAlignedAlloc), BIND("strerror", linuxExtraStrerror),
    BIND("strtoimax", linuxExtraStrtoimax), BIND("__getauxval", linuxRuntimeGetauxval), BIND("_setjmp", setjmp), BIND("longjmp", longjmp), BIND("qsort", qsort),
    BIND("rand", rand), BIND("memchr", memchr), BIND("strspn", strspn), BIND("stat", linuxExtraStat), BIND("stat64", linuxExtraStat),
    BIND("lstat", linuxExtraLstat), BIND("lstat64", linuxExtraLstat), BIND("fstat", linuxExtraFstat), BIND("fstat64", linuxExtraFstat),
    BIND("fcntl64", linuxAbiFcntl), BIND("ftruncate", linuxAbiFtruncate), BIND("truncate", linuxExtraTruncate), BIND("openat", linuxExtraOpenat),
    BIND("openat64", linuxExtraOpenat), BIND("unlinkat", linuxExtraUnlinkat), BIND("fchmod", linuxExtraZero), BIND("fchmodat", linuxExtraZero),
    BIND("utimensat", linuxExtraZero), BIND("futimens", linuxExtraZero), BIND("link", linuxExtraRefuseNosys), BIND("symlink", linuxExtraRefuseNosys),
    BIND("statfs", linuxExtraRefuseNosys), BIND("statvfs", linuxExtraRefuseNosys), BIND("fstatfs", linuxExtraRefuseNosys),
    BIND("memfd_create", linuxExtraRefuseNosys), BIND("ioctl", linuxAbiIoctl),
    // ---- sockets (linux_net.c): IPv4 over the BSD service
    BIND("msync", linuxAbiMsync), BIND("eventfd", linuxAbiEventfd), BIND("pipe", linuxAbiPipe), BIND("pipe2", linuxAbiPipe2),
    BIND("epoll_create", linuxAbiEpollCreate), BIND("epoll_create1", linuxAbiEpollCreate1), BIND("epoll_ctl", linuxAbiEpollCtl),
    BIND("epoll_wait", linuxAbiEpollWait), BIND("epoll_pwait", linuxAbiEpollPwait), BIND("gethostname", linuxAbiGethostname), BIND("socket", linuxAbiSocket),
    BIND("connect", linuxAbiConnect), BIND("bind", linuxAbiBind), BIND("listen", linuxAbiListen), BIND("accept", linuxAbiAccept),
    BIND("accept4", linuxAbiAccept4), BIND("getsockname", linuxAbiGetsockname), BIND("getpeername", linuxAbiGetpeername),
    BIND("setsockopt", linuxAbiSetsockopt), BIND("getsockopt", linuxAbiGetsockopt), BIND("shutdown", linuxAbiShutdown), BIND("send", linuxAbiSend),
    BIND("recv", linuxAbiRecv), BIND("sendto", linuxAbiSendto), BIND("recvfrom", linuxAbiRecvfrom), BIND("poll", linuxAbiPoll),
    BIND("getaddrinfo", linuxAbiGetaddrinfo), BIND("freeaddrinfo", linuxAbiFreeaddrinfo), BIND("gai_strerror", linuxAbiGaiStrerror),
    BIND("inet_pton", linuxAbiInetPton), BIND("dirfd", linuxExtraRefuseNoentFd), BIND("fdopendir", linuxExtraRefuseNoentFd),
    BIND("readdir", linuxAbiReaddir64)};
#define BINDO(name, object) {name, (uintptr_t)&(object), "GLIBC_2.17", "libc.so.6"}
#define BINDOL(name, object, library) {name, (uintptr_t)&(object), "GLIBC_2.17", library}
// environ/__environ are the address of one pointer object; __stack_chk_guard is a value object
// that glibc provides from the dynamic loader, not from libc.so.6.
static const Binding objects[] = {BINDO("stdout", linuxStdioStdout),
                                  BINDO("stderr", linuxStdioStderr),
                                  BINDO("environ", linuxRuntimeEnviron),
                                  BINDO("__environ", linuxRuntimeEnviron),
                                  BINDOL("__stack_chk_guard", linuxRuntimeStackGuard, "ld-linux-aarch64.so.1"),
                                  BINDO("stdin", linuxExtraStdin),
                                  BINDO("__libc_single_threaded", linuxExtraSingleThreaded)};
bool linuxAbiLookup(const char *name, uintptr_t *address) {
    if (!name || !address) return false;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); ++i)
        if (!strcmp(name, bindings[i].name)) {
            *address = bindings[i].address;
            return true;
        }
    linuxRuntimeInitialize();
    for (size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); ++i)
        if (!strcmp(name, objects[i].name)) {
            *address = objects[i].address;
            return true;
        }
    return false;
}
bool linuxAbiResolve(void *context, const ElfImport *symbol, uintptr_t *address) {
    (void)context;
    *address = 0;
    if (symbol->type != STT_FUNC && symbol->type != STT_OBJECT) return false;
    if (symbol->type == STT_OBJECT) {
        if (!symbol->version || !symbol->library) return false;
        for (size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); ++i)
            if (!strcmp(symbol->name, objects[i].name) && !strcmp(symbol->version, objects[i].version) && !strcmp(symbol->library, objects[i].library)) {
                linuxRuntimeInitialize();  // the guard and environment must exist before any client frame reads them
                *address = objects[i].address;
                return true;
            }
        return false;
    }
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); ++i) {
        const Binding *b = &bindings[i];
        bool version = b->version ? symbol->version && !strcmp(symbol->version, b->version) : !symbol->version;
        // Legacy zlib imports have no provider/version metadata. Only this
        // explicit whitelist supplies them; this is not a dynamic link scope.
        bool library = symbol->library ? !strcmp(symbol->library, b->library) : !b->version && !strcmp(b->library, "libz.so.1");
        if (!strcmp(symbol->name, b->name) && version && library) {
            *address = b->address;
            return true;
        }
    }
    return false;
}
