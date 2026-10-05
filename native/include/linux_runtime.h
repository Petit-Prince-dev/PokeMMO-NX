#pragma once
#include "linux_abi.h"
#include "linux_threads.h"

// Linux AArch64 glibc layouts (LP64). Fixed-width fields; sizes are asserted.
typedef struct {
    uint64_t words[16];
} LinuxSigset;  // sigset_t, 128 bytes
typedef struct {
    uint64_t handler;
    LinuxSigset mask;
    int32_t flags, pad;
    uint64_t restorer;
} LinuxSigaction;  // 152 bytes
typedef struct {
    uint64_t current, maximum;
} LinuxRlimit;  // struct rlimit/rlimit64, 16 bytes
typedef struct {
    char *name, *passwd;
    uint32_t uid, gid;
    char *gecos, *dir, *shell;
} LinuxPasswd;  // 48 bytes
typedef struct {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
} LinuxUtsname;  // 390 bytes
typedef struct {
    const char *name;
    uintptr_t address;
} LinuxRuntimeExport;
typedef enum {
    LINUX_TERMINATION_NONE = 0,
    LINUX_TERMINATION_EXIT = 1,
    LINUX_TERMINATION_EXIT_IMMEDIATE = 2,
    LINUX_TERMINATION_ABORT = 3,
    LINUX_TERMINATION_STACK_SMASH = 4,
    LINUX_TERMINATION_UNRESOLVED_IMPORT = 5  // status is the trap index of the import that has no adapter
} LinuxTerminationKind;
// First request wins (kind/status/thread); requests counts every attempt since the last clear.
typedef struct {
    LinuxTerminationKind kind;
    int status;
    uint64_t thread, requests;
    uintptr_t caller;
} LinuxTermination;
LinuxTerminationKind linuxRuntimeTerminationKindUnlocked(void);  // for watchers that must never wait for a lock
typedef void *(*LinuxRuntimeEntry)(void *);

#define LINUX_SIGNAL_MAX 64
#define LINUX_ENV_MAX_ENTRIES 64u
#define LINUX_ENV_MAX_BYTES 4096u
#define LINUX_THREAD_NAME_MAX 15u
#define LINUX_RLIMIT_COUNT 16u
#define LINUX_RLIM_INFINITY UINT64_MAX
#define LINUX_UID 1000u
#define LINUX_HOME "/home/switch"
enum { LINUX_MREMAP_MAYMOVE = 1, LINUX_MREMAP_FIXED = 2, LINUX_MREMAP_DONTUNMAP = 4 };
enum { LINUX_RTLD_LAZY = 1, LINUX_RTLD_NOW = 2, LINUX_RTLD_NOLOAD = 4, LINUX_RTLD_DEEPBIND = 8, LINUX_RTLD_GLOBAL = 0x100, LINUX_RTLD_NODELETE = 0x1000 };

// Locale: only the C locale exists. "C", "POSIX" and "" (resolved through LC_ALL, the category variable
// and LANG, default "C") select it; any other name fails with EINVAL, as glibc does for a missing locale.
// A NULL name queries. The result is a static string and is always "C" on success.
#define LINUX_LC_IDENTIFICATION 12  // the last of glibc's locale categories (0..12)
char *linuxRuntimeSetlocale(int category, const char *locale);

// syscall(2): only SYS_gettid (178, AArch64) is provided; it returns the managed thread id. Any other number
// fails with ENOSYS and is recorded (number and call count) for the diagnostics.
#define LINUX_SYS_GETTID 178
typedef struct {
    int64_t number;
    uint64_t calls;
} LinuxSyscallRecord;
long linuxRuntimeSyscall(long number);
size_t linuxRuntimeUnsupportedSyscalls(LinuxSyscallRecord *out, size_t maximum);

// Auxiliary vector (getauxval). The hardware capabilities describe the Cortex-A57 cores of the
// Switch: FP, ASIMD, AES, PMULL, SHA1, SHA2 and CRC32; no LSE atomics, SVE or event stream.
enum {
    LINUX_AT_PAGESZ = 6,
    LINUX_AT_UID = 11,
    LINUX_AT_EUID = 12,
    LINUX_AT_GID = 13,
    LINUX_AT_EGID = 14,
    LINUX_AT_PLATFORM = 15,
    LINUX_AT_HWCAP = 16,
    LINUX_AT_CLKTCK = 17,
    LINUX_AT_SECURE = 23,
    LINUX_AT_RANDOM = 25,
    LINUX_AT_HWCAP2 = 26
};
#define LINUX_HWCAP_A57 UINT64_C(0xfb)          // FP|ASIMD|AES|PMULL|SHA1|SHA2|CRC32
uint64_t linuxRuntimeGetauxval(uint64_t type);  // unknown types return 0 with ENOENT; known ones leave errno untouched

// Process environment: a fixed table; getenv reads the pointer the program sees.
extern char **linuxRuntimeEnviron;        // glibc environ / __environ (pointer object)
extern uintptr_t linuxRuntimeStackGuard;  // glibc __stack_chk_guard (value object, low byte zero)
void linuxRuntimeInitialize(void);        // idempotent: default environment and random stack guard
char *linuxRuntimeGetenv(const char *name);

// Identity: one virtual user. Horizon has no uid/gid model. Functions returning
// an error number leave errno untouched; the others set the Linux errno TLS.
uint32_t linuxRuntimeGetuid(void);
int linuxRuntimeGetpwuidR(uint32_t uid, LinuxPasswd *pwd, char *buffer, size_t bytes, LinuxPasswd **result);
int linuxRuntimeUname(LinuxUtsname *out);
int linuxRuntimeGetrlimit(int resource, LinuxRlimit *out);
int linuxRuntimeSetrlimit(int resource, const LinuxRlimit *limit);
char *linuxRuntimeStrerrorR(int error, char *buffer, size_t bytes);   // GNU variant: returns the message
int linuxRuntimeXpgStrerrorR(int error, char *buffer, size_t bytes);  // XSI variant: returns 0/EINVAL/ERANGE
char *linuxRuntimeRealpath(const char *path, char *resolved);         // NULL resolved allocates; buffer assumed PATH_MAX
// The program image as the process sees it: /proc/self/exe resolves to this virtual path (realpath, readlink).
#define LINUX_EXE_PATH "/game/bin/linux/arm64/PokeMMO"
int64_t linuxRuntimeReadlink(const char *path, char *buffer, size_t size);  // the virtual root has no symlinks except /proc/self/exe
int linuxRuntimeSigrtmax(void);                                             // __libc_current_sigrtmax: 64
int linuxRuntimeSetThreadName(LinuxPthread thread, const char *name);
bool linuxRuntimeGetThreadName(LinuxPthread thread, char out[LINUX_THREAD_NAME_MAX + 1]);  // diagnostics

// Signals: dispositions and per-thread masks are recorded only. No signal is ever delivered.
int linuxRuntimeSigemptyset(LinuxSigset *set);
int linuxRuntimeSigaddset(LinuxSigset *set, int signal);
int linuxRuntimeSigprocmask(int how, const LinuxSigset *set, LinuxSigset *old);
int linuxRuntimeSigaction(int signal, const LinuxSigaction *action, LinuxSigaction *old);

// Memory remapping: no operation is supported. MREMAP_DONTUNMAP reports EINVAL like a kernel before 5.7.
void *linuxRuntimeMremap(void *old_address, size_t old_size, size_t new_size, int flags, void *new_address);

// Termination. The adapter records the request and unwinds the calling thread to the
// innermost linuxRuntimeRun context. Other threads keep running.
// A thread with no armed context is a contract violation and ends the process.
void linuxRuntimeExit(int status) __attribute__((noreturn));           // flushes managed stdio first
void linuxRuntimeExitImmediate(int status) __attribute__((noreturn));  // _exit: no flush
void linuxRuntimeAbort(void) __attribute__((noreturn));
void linuxRuntimeStackCheckFail(void) __attribute__((noreturn));
void linuxRuntimeUnresolvedImport(unsigned index) __attribute__((noreturn));  // a client call reached an import without adapter
LinuxTermination linuxRuntimeTermination(void);
// Runs entry on this thread with termination unwinding armed. True when entry returned,
// false when exit/abort/stack-check unwound it (result untouched). Managed pthreads use it.
bool linuxRuntimeRun(LinuxRuntimeEntry entry, void *argument, void **result);

// Dynamic lookup: the main program exports and the adapter registry, then the library loader below.
// Without a loader no library can be opened: unknown libraries return NULL without errno.
void *linuxRuntimeDlopen(const char *file, int flags);
void *linuxRuntimeDlsym(void *handle, const char *name);
void linuxRuntimeSetExports(const LinuxRuntimeExport *table, size_t count);  // table must outlive its use
// Real shared libraries (installed by the game, see linux_dl.c): dlopen of anything the registry does not provide,
// dlsym on their handles and in the default scope, dlclose, dlerror and dl_iterate_phdr go through these hooks.
typedef struct {
    void *(*open)(const char *file, int flags);
    void *(*symbol)(void *handle, const char *name);  // handle NULL: every loaded library
    int (*close)(void *handle);
    const char *(*error)(void);
    int (*iterate)(int (*callback)(void *info, size_t size, void *data), void *data);
} LinuxDynamicLoader;
void linuxRuntimeSetDynamicLoader(const LinuxDynamicLoader *loader);  // NULL removes it; cleared by linuxRuntimeReset
int linuxRuntimeDlclose(void *handle);
const char *linuxRuntimeDlerror(void);
int linuxRuntimeDlIteratePhdr(int (*callback)(void *info, size_t size, void *data), void *data);
int linuxRuntimeDladdr(const void *address, void *info);  // not provided: always 0

// Restore defaults: environment, limits, dispositions, thread names, exports, termination and this
// thread's signal mask. The stack guard value is kept. Callers must be quiescent.
void linuxRuntimeReset(void);
