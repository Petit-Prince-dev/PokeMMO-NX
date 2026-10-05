#include "linux_runtime.h"
#include "linux_directories.h"
#include "linux_files.h"
#include "linux_stdio.h"
#include "linux_vm.h"
#include <setjmp.h>
#include <stdatomic.h>
#include <string.h>
#include <switch.h>

_Static_assert(sizeof(LinuxSigset) == 128 && sizeof(LinuxSigaction) == 152 && offsetof(LinuxSigaction, flags) == 136 &&
                   offsetof(LinuxSigaction, restorer) == 144,
               "glibc AArch64 sigaction layout");
_Static_assert(sizeof(LinuxRlimit) == 16 && sizeof(LinuxPasswd) == 48 && offsetof(LinuxPasswd, uid) == 16 && offsetof(LinuxPasswd, gecos) == 24 &&
                   sizeof(LinuxUtsname) == 390,
               "glibc LP64 layouts");

static int fail(int error) {
    *linuxAbiErrnoLocation() = error;
    return -1;
}

// One short non-blocking critical section at a time; never held across a callback or I/O.
static atomic_flag state_lock = ATOMIC_FLAG_INIT;
static void lock(void) {
    while (atomic_flag_test_and_set_explicit(&state_lock, memory_order_acquire)) linuxThreadYield();
}
static void unlock(void) { atomic_flag_clear_explicit(&state_lock, memory_order_release); }

// ---- environment and stack guard ------------------------------------------------------------
static char environment_storage[LINUX_ENV_MAX_BYTES];
static char *environment_table[LINUX_ENV_MAX_ENTRIES + 1];
char **linuxRuntimeEnviron = environment_table;
uintptr_t linuxRuntimeStackGuard;
static _Atomic bool initialized;
static unsigned char auxv_random[16];  // AT_RANDOM points here; filled once at initialization
static const char *const default_environment[] = {
    // HOME must equal LINUX_HOME
    "HOME=/home/switch", "USER=switch", "LOGNAME=switch",      "PATH=/usr/bin:/bin", "LANG=C",
    "TMPDIR=/tmp",       "DISPLAY=:0",  "XDG_SESSION_TYPE=x11"  // the game only starts when it believes a Linux display server is present
};

static bool storeEnvironment(const char *const *entries, size_t count) {
    if (count > LINUX_ENV_MAX_ENTRIES || (count && !entries)) return false;
    char staging[LINUX_ENV_MAX_BYTES];  // entries may alias the live table
    size_t bytes = 0;
    for (size_t i = 0; i < count; ++i) {
        const char *entry = entries[i];
        const char *equals = entry ? strchr(entry, '=') : NULL;
        if (!equals || equals == entry) return false;
        size_t need = strlen(entry) + 1;
        if (need > LINUX_ENV_MAX_BYTES - bytes) return false;
        memcpy(staging + bytes, entry, need);
        bytes += need;
    }
    memcpy(environment_storage, staging, bytes);
    size_t offset = 0;
    for (size_t i = 0; i < count; ++i) {
        environment_table[i] = environment_storage + offset;
        offset += strlen(environment_storage + offset) + 1;
    }
    for (size_t i = count; i <= LINUX_ENV_MAX_ENTRIES; ++i) environment_table[i] = NULL;
    linuxRuntimeEnviron = environment_table;
    return true;
}
static uintptr_t makeGuard(void) {
    uint64_t value = 0;
    randomGet(&value, sizeof(value));
    value &= ~UINT64_C(0xff);  // glibc keeps a zero terminator byte in the canary
    return (uintptr_t)(value ? value : UINT64_C(0x504f4b454d4d4f00));
}
void linuxRuntimeInitialize(void) {
    if (atomic_load_explicit(&initialized, memory_order_acquire)) return;
    lock();
    if (!atomic_load_explicit(&initialized, memory_order_relaxed)) {
        storeEnvironment(default_environment, sizeof(default_environment) / sizeof(default_environment[0]));
        if (!linuxRuntimeStackGuard) linuxRuntimeStackGuard = makeGuard();
        randomGet(auxv_random, sizeof(auxv_random));
        atomic_store_explicit(&initialized, true, memory_order_release);
    }
    unlock();
}
char *linuxRuntimeGetenv(const char *name) {
    linuxRuntimeInitialize();
    if (!name || !*name || strchr(name, '=')) return NULL;
    size_t length = strlen(name);
    char *found = NULL;
    lock();
    for (char **entry = linuxRuntimeEnviron; entry && *entry; ++entry)
        if (!strncmp(*entry, name, length) && (*entry)[length] == '=') {
            found = *entry + length + 1;
            break;
        }
    unlock();
    return found;
}

// ---- locale -----------------------------------------------------------------------------------
static bool isCName(const char *name) { return !strcmp(name, "C") || !strcmp(name, "POSIX"); }
char *linuxRuntimeSetlocale(int category, const char *locale) {
    static char c_name[] = "C";
    if (category < 0 || category > LINUX_LC_IDENTIFICATION) {
        fail(LINUX_EINVAL);
        return NULL;
    }
    if (!locale) return c_name;
    if (!*locale) {
        // "" means: the environment decides. LC_ALL overrides everything, then the category variable, then LANG.
        static const char *const variables[] = {"LC_CTYPE", "LC_NUMERIC", "LC_TIME",    "LC_COLLATE",   "LC_MONETARY",    "LC_MESSAGES",      NULL,
                                                "LC_PAPER", "LC_NAME",    "LC_ADDRESS", "LC_TELEPHONE", "LC_MEASUREMENT", "LC_IDENTIFICATION"};
        const char *name = linuxRuntimeGetenv("LC_ALL");
        if (!name || !*name) name = variables[category] ? linuxRuntimeGetenv(variables[category]) : NULL;
        if (!name || !*name) name = linuxRuntimeGetenv("LANG");
        if (!name || !*name) return c_name;
        locale = name;
    }
    if (isCName(locale)) return c_name;
    fail(LINUX_EINVAL);
    return NULL;
}

// ---- auxiliary vector -------------------------------------------------------------------------
uint64_t linuxRuntimeGetauxval(uint64_t type) {
    linuxRuntimeInitialize();
    switch (type) {
        case LINUX_AT_PAGESZ: return LINUX_VM_PAGE;
        case LINUX_AT_UID:
        case LINUX_AT_EUID:
        case LINUX_AT_GID:
        case LINUX_AT_EGID: return LINUX_UID;
        case LINUX_AT_PLATFORM: return (uint64_t)(uintptr_t)"aarch64";
        case LINUX_AT_HWCAP: return LINUX_HWCAP_A57;
        case LINUX_AT_CLKTCK: return 100;
        case LINUX_AT_SECURE:
        case LINUX_AT_HWCAP2: return 0;
        case LINUX_AT_RANDOM: return (uint64_t)(uintptr_t)auxv_random;
        default: *linuxAbiErrnoLocation() = LINUX_ENOENT; return 0;
    }
}

// ---- identity -------------------------------------------------------------------------------
uint32_t linuxRuntimeGetuid(void) { return LINUX_UID; }
int linuxRuntimeGetpwuidR(uint32_t uid, LinuxPasswd *pwd, char *buffer, size_t bytes, LinuxPasswd **result) {
    if (!result) return LINUX_EFAULT;
    *result = NULL;
    if (!pwd || (bytes && !buffer)) return LINUX_EFAULT;
    if (uid != LINUX_UID) return 0;  // not found is success with a NULL result
    static const char *const fields[] = {"switch", "x", "Nintendo Switch", LINUX_HOME, "/bin/sh"};
    size_t need = 0;
    for (unsigned i = 0; i < 5; ++i) need += strlen(fields[i]) + 1;
    if (bytes < need) return LINUX_ERANGE;
    char *cursor = buffer, *copies[5];
    for (unsigned i = 0; i < 5; ++i) {
        size_t n = strlen(fields[i]) + 1;
        memcpy(cursor, fields[i], n);
        copies[i] = cursor;
        cursor += n;
    }
    *pwd = (LinuxPasswd){.name = copies[0], .passwd = copies[1], .uid = LINUX_UID, .gid = LINUX_UID, .gecos = copies[2], .dir = copies[3], .shell = copies[4]};
    *result = pwd;
    return 0;
}
int linuxRuntimeUname(LinuxUtsname *out) {
    if (!out) return fail(LINUX_EFAULT);
    LinuxUtsname value;
    memset(&value, 0, sizeof(value));
    // An emulated identity: the kernel is Horizon. A 4.9 release matches the mremap contract below.
    strcpy(value.sysname, "Linux");
    strcpy(value.nodename, "switch");
    strcpy(value.release, "4.9.140");
    strcpy(value.version, "#1 SMP PREEMPT Horizon");
    strcpy(value.machine, "aarch64");
    strcpy(value.domainname, "(none)");
    *out = value;
    return 0;
}

// ---- resource limits (recorded; only NOFILE reflects a real capacity) -------------------------
#define UNLIMITED {LINUX_RLIM_INFINITY, LINUX_RLIM_INFINITY}
static const LinuxRlimit default_limits[LINUX_RLIMIT_COUNT] = {
    UNLIMITED,
    UNLIMITED,
    UNLIMITED,                                  // CPU, FSIZE, DATA
    {8u * 1024u * 1024u, LINUX_RLIM_INFINITY},  // STACK: nominal Linux default
    {0, LINUX_RLIM_INFINITY},
    UNLIMITED,                                                           // CORE, RSS
    {LINUX_THREAD_MAX_THREADS, LINUX_THREAD_MAX_THREADS},                // NPROC: managed thread capacity
    {LINUX_FILE_MAX_DESCRIPTORS + 3u, LINUX_FILE_MAX_DESCRIPTORS + 3u},  // NOFILE: descriptors 0..130
    {65536, 65536},
    UNLIMITED,
    UNLIMITED,  // MEMLOCK, AS, LOCKS
    {0, 0},
    {819200, 819200},
    {0, 0},
    {0, 0},
    UNLIMITED  // SIGPENDING, MSGQUEUE, NICE, RTPRIO, RTTIME
};
static LinuxRlimit limits[LINUX_RLIMIT_COUNT];
static bool limits_ready;
static void readyLimits(void) {
    if (!limits_ready) {
        memcpy(limits, default_limits, sizeof(limits));
        limits_ready = true;
    }
}
int linuxRuntimeGetrlimit(int resource, LinuxRlimit *out) {
    if (resource < 0 || (unsigned)resource >= LINUX_RLIMIT_COUNT) return fail(LINUX_EINVAL);
    if (!out) return fail(LINUX_EFAULT);
    lock();
    readyLimits();
    LinuxRlimit value = limits[resource];
    unlock();
    *out = value;
    return 0;
}
int linuxRuntimeSetrlimit(int resource, const LinuxRlimit *limit) {
    if (resource < 0 || (unsigned)resource >= LINUX_RLIMIT_COUNT) return fail(LINUX_EINVAL);
    if (!limit) return fail(LINUX_EFAULT);
    LinuxRlimit next = *limit;
    if (next.current > next.maximum) return fail(LINUX_EINVAL);
    lock();
    readyLimits();
    int error = next.maximum > limits[resource].maximum ? LINUX_EPERM : 0;  // unprivileged: the hard limit only shrinks
    if (!error) limits[resource] = next;
    unlock();
    return error ? fail(error) : 0;
}

// ---- strerror -------------------------------------------------------------------------------
// glibc 2.34 messages for the Linux errno numbers; the two unassigned numbers stay NULL.
static const char *const messages[] = {[0] = "Success",
                                       [1] = "Operation not permitted",
                                       [2] = "No such file or directory",
                                       [3] = "No such process",
                                       [4] = "Interrupted system call",
                                       [5] = "Input/output error",
                                       [6] = "No such device or address",
                                       [7] = "Argument list too long",
                                       [8] = "Exec format error",
                                       [9] = "Bad file descriptor",
                                       [10] = "No child processes",
                                       [11] = "Resource temporarily unavailable",
                                       [12] = "Cannot allocate memory",
                                       [13] = "Permission denied",
                                       [14] = "Bad address",
                                       [15] = "Block device required",
                                       [16] = "Device or resource busy",
                                       [17] = "File exists",
                                       [18] = "Invalid cross-device link",
                                       [19] = "No such device",
                                       [20] = "Not a directory",
                                       [21] = "Is a directory",
                                       [22] = "Invalid argument",
                                       [23] = "Too many open files in system",
                                       [24] = "Too many open files",
                                       [25] = "Inappropriate ioctl for device",
                                       [26] = "Text file busy",
                                       [27] = "File too large",
                                       [28] = "No space left on device",
                                       [29] = "Illegal seek",
                                       [30] = "Read-only file system",
                                       [31] = "Too many links",
                                       [32] = "Broken pipe",
                                       [33] = "Numerical argument out of domain",
                                       [34] = "Numerical result out of range",
                                       [35] = "Resource deadlock avoided",
                                       [36] = "File name too long",
                                       [37] = "No locks available",
                                       [38] = "Function not implemented",
                                       [39] = "Directory not empty",
                                       [40] = "Too many levels of symbolic links",
                                       [42] = "No message of desired type",
                                       [43] = "Identifier removed",
                                       [44] = "Channel number out of range",
                                       [45] = "Level 2 not synchronized",
                                       [46] = "Level 3 halted",
                                       [47] = "Level 3 reset",
                                       [48] = "Link number out of range",
                                       [49] = "Protocol driver not attached",
                                       [50] = "No CSI structure available",
                                       [51] = "Level 2 halted",
                                       [52] = "Invalid exchange",
                                       [53] = "Invalid request descriptor",
                                       [54] = "Exchange full",
                                       [55] = "No anode",
                                       [56] = "Invalid request code",
                                       [57] = "Invalid slot",
                                       [59] = "Bad font file format",
                                       [60] = "Device not a stream",
                                       [61] = "No data available",
                                       [62] = "Timer expired",
                                       [63] = "Out of streams resources",
                                       [64] = "Machine is not on the network",
                                       [65] = "Package not installed",
                                       [66] = "Object is remote",
                                       [67] = "Link has been severed",
                                       [68] = "Advertise error",
                                       [69] = "Srmount error",
                                       [70] = "Communication error on send",
                                       [71] = "Protocol error",
                                       [72] = "Multihop attempted",
                                       [73] = "RFS specific error",
                                       [74] = "Bad message",
                                       [75] = "Value too large for defined data type",
                                       [76] = "Name not unique on network",
                                       [77] = "File descriptor in bad state",
                                       [78] = "Remote address changed",
                                       [79] = "Can not access a needed shared library",
                                       [80] = "Accessing a corrupted shared library",
                                       [81] = ".lib section in a.out corrupted",
                                       [82] = "Attempting to link in too many shared libraries",
                                       [83] = "Cannot exec a shared library directly",
                                       [84] = "Invalid or incomplete multibyte or wide character",
                                       [85] = "Interrupted system call should be restarted",
                                       [86] = "Streams pipe error",
                                       [87] = "Too many users",
                                       [88] = "Socket operation on non-socket",
                                       [89] = "Destination address required",
                                       [90] = "Message too long",
                                       [91] = "Protocol wrong type for socket",
                                       [92] = "Protocol not available",
                                       [93] = "Protocol not supported",
                                       [94] = "Socket type not supported",
                                       [95] = "Operation not supported",
                                       [96] = "Protocol family not supported",
                                       [97] = "Address family not supported by protocol",
                                       [98] = "Address already in use",
                                       [99] = "Cannot assign requested address",
                                       [100] = "Network is down",
                                       [101] = "Network is unreachable",
                                       [102] = "Network dropped connection on reset",
                                       [103] = "Software caused connection abort",
                                       [104] = "Connection reset by peer",
                                       [105] = "No buffer space available",
                                       [106] = "Transport endpoint is already connected",
                                       [107] = "Transport endpoint is not connected",
                                       [108] = "Cannot send after transport endpoint shutdown",
                                       [109] = "Too many references: cannot splice",
                                       [110] = "Connection timed out",
                                       [111] = "Connection refused",
                                       [112] = "Host is down",
                                       [113] = "No route to host",
                                       [114] = "Operation already in progress",
                                       [115] = "Operation now in progress",
                                       [116] = "Stale file handle",
                                       [117] = "Structure needs cleaning",
                                       [118] = "Not a XENIX named type file",
                                       [119] = "No XENIX semaphores available",
                                       [120] = "Is a named type file",
                                       [121] = "Remote I/O error",
                                       [122] = "Disk quota exceeded",
                                       [123] = "No medium found",
                                       [124] = "Wrong medium type",
                                       [125] = "Operation canceled",
                                       [126] = "Required key not available",
                                       [127] = "Key has expired",
                                       [128] = "Key has been revoked",
                                       [129] = "Key was rejected by service",
                                       [130] = "Owner died",
                                       [131] = "State not recoverable",
                                       [132] = "Operation not possible due to RF-kill",
                                       [133] = "Memory page has hardware error"};
static const char *message(int error) { return error >= 0 && (size_t)error < sizeof(messages) / sizeof(messages[0]) ? messages[error] : NULL; }
static void unknownError(char *buffer, size_t bytes, int error) {
    char digits[16];
    unsigned length = 0;
    unsigned value = error < 0 ? 0u - (unsigned)error : (unsigned)error;
    do {
        digits[length++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    char text[40];
    size_t used = 0;
    static const char prefix[] = "Unknown error ";
    memcpy(text, prefix, sizeof(prefix) - 1);
    used = sizeof(prefix) - 1;
    if (error < 0) text[used++] = '-';
    while (length) text[used++] = digits[--length];
    text[used] = 0;
    if (bytes) {
        size_t copy = used < bytes - 1 ? used : bytes - 1;
        memcpy(buffer, text, copy);
        buffer[copy] = 0;
    }
}
char *linuxRuntimeStrerrorR(int error, char *buffer, size_t bytes) {
    const char *text = message(error);
    if (text) return (char *)text;
    unknownError(buffer, bytes, error);
    return bytes ? buffer : (char *)"Unknown error";
}
int linuxRuntimeXpgStrerrorR(int error, char *buffer, size_t bytes) {
    const char *text = message(error);
    if (!text) {
        unknownError(buffer, bytes, error);
        return LINUX_EINVAL;
    }
    size_t length = strlen(text);
    if (bytes) {
        size_t copy = length < bytes - 1 ? length : bytes - 1;
        memcpy(buffer, text, copy);
        buffer[copy] = 0;
    }
    return length >= bytes ? LINUX_ERANGE : 0;
}

// ---- realpath -------------------------------------------------------------------------------
// The virtual root has no symbolic links, so lexical normalization is exact; every prefix is then
// verified through the file adapters (ENOENT, ENOTDIR and /proc,/sys,/dev refusals come from them).
int linuxRuntimeSigrtmax(void) { return 64; }
int64_t linuxRuntimeReadlink(const char *path, char *buffer, size_t size) {
    if (!path || !buffer) {
        fail(LINUX_EFAULT);
        return -1;
    }
    if (!*path) {
        fail(LINUX_ENOENT);
        return -1;
    }
    if (!size || size > (size_t)INT64_MAX) {
        fail(LINUX_EINVAL);
        return -1;
    }
    if (!strcmp(path, "/proc/self/exe")) {
        size_t length = strlen(LINUX_EXE_PATH);
        if (length > size) length = size;  // readlink never terminates the string
        memcpy(buffer, LINUX_EXE_PATH, length);
        return (int64_t)length;
    }
    LinuxFileStat status;
    if (linuxAbiXstat(0, path, &status)) return -1;
    fail(LINUX_EINVAL);  // it exists and is not a symbolic link
    return -1;
}
char *linuxRuntimeRealpath(const char *path, char *resolved) {
    if (!path) {
        fail(LINUX_EINVAL);
        return NULL;
    }
    if (!*path) {
        fail(LINUX_ENOENT);
        return NULL;
    }
    if (!strcmp(path, "/proc/self/exe")) path = LINUX_EXE_PATH;
    size_t input = strlen(path);
    if (input >= LINUX_FILE_PATH_MAX) {
        fail(LINUX_ENAMETOOLONG);
        return NULL;
    }
    char normalized[LINUX_FILE_PATH_MAX];
    size_t length = 0;
    LinuxFileStat status;
    if (linuxAbiXstat(0, "/", &status)) return NULL;
    if (path[0] != '/') {
        if (!linuxAbiGetcwd(normalized, sizeof(normalized))) return NULL;
        length = normalized[1] ? strlen(normalized) : 0;  // root is the empty prefix
    }
    // `verified` is the longest prefix known to be an existing directory (the root and the cwd are).
    // Every component, "." and ".." included, is entered only from a verified directory, as on Linux:
    // "/file/.." is ENOTDIR and "/missing/.." is ENOENT even though it collapses lexically.
    size_t verified = length;
    for (const char *cursor = path; *cursor;) {
        while (*cursor == '/') ++cursor;
        const char *begin = cursor;
        while (*cursor && *cursor != '/') ++cursor;
        size_t count = (size_t)(cursor - begin);
        if (!count) continue;
        if (length > verified) {
            char saved = normalized[length];
            normalized[length] = 0;
            int rc = linuxAbiXstat(0, normalized, &status);
            normalized[length] = saved;
            if (rc) return NULL;
            if ((status.mode & 0170000) != 0040000) {
                fail(LINUX_ENOTDIR);
                return NULL;
            }
            verified = length;
        }
        if (count == 1 && begin[0] == '.') continue;
        if (count == 2 && begin[0] == '.' && begin[1] == '.') {
            while (length && normalized[--length] != '/') {}
            if (verified > length) verified = length;  // an ancestor of a verified directory is one too
            continue;
        }
        if (length + 1 + count >= sizeof(normalized)) {
            fail(LINUX_ENAMETOOLONG);
            return NULL;
        }
        normalized[length++] = '/';
        memcpy(normalized + length, begin, count);
        length += count;
    }
    if (length > verified) {  // the final component must exist; a trailing slash also requires a directory
        char saved = normalized[length];
        normalized[length] = 0;
        int rc = linuxAbiXstat(0, normalized, &status);
        normalized[length] = saved;
        if (rc) return NULL;
        if (path[input - 1] == '/' && (status.mode & 0170000) != 0040000) {
            fail(LINUX_ENOTDIR);
            return NULL;
        }
    }
    if (!length) normalized[length++] = '/';
    normalized[length] = 0;
    if (!resolved) {
        resolved = linuxAbiMalloc(length + 1);
        if (!resolved) return NULL;
    }
    memcpy(resolved, normalized, length + 1);
    return resolved;
}

// ---- thread names (diagnostics only; Horizon has no kernel thread names) ----------------------
// A managed thread's name is reclaimed once it was joined; names of foreign threads (the main
// thread, for instance) cannot be validated from another thread and stay until reset.
typedef struct {
    LinuxPthread id;
    bool managed;
    char name[LINUX_THREAD_NAME_MAX + 1];
} NameSlot;
static NameSlot names[LINUX_THREAD_MAX_THREADS + 1];
int linuxRuntimeSetThreadName(LinuxPthread thread, const char *name) {
    if (!name) return LINUX_EFAULT;
    size_t length = strlen(name);
    if (length > LINUX_THREAD_NAME_MAX) return LINUX_ERANGE;
    bool managed = linuxPthreadManaged(thread);
    if (!managed && !linuxPthreadExists(thread)) return LINUX_ESRCH;
    lock();
    NameSlot *slot = NULL, *spare = NULL;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (names[i].id == thread)
            slot = &names[i];
        else if (!names[i].id && !spare)
            spare = &names[i];
    }
    if (!slot && !spare)  // reclaim names of threads that were joined
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
            if (names[i].managed && !linuxPthreadManaged(names[i].id)) {
                memset(&names[i], 0, sizeof(names[i]));
                if (!spare) spare = &names[i];
            }
    if (!slot) slot = spare;
    int error = slot ? 0 : LINUX_EAGAIN;
    if (slot) {
        slot->id = thread;
        slot->managed = managed;
        memcpy(slot->name, name, length + 1);
    }
    unlock();
    return error;
}
bool linuxRuntimeGetThreadName(LinuxPthread thread, char out[LINUX_THREAD_NAME_MAX + 1]) {
    bool found = false;
    lock();
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]) && !found; ++i)
        if (thread && names[i].id == thread) {
            memcpy(out, names[i].name, sizeof(names[i].name));
            found = true;
        }
    unlock();
    return found;
}

// ---- signals: recorded only -------------------------------------------------------------------
enum { SIGNAL_KILL = 9, SIGNAL_STOP = 19, SIGNAL_CANCEL = 32, SIGNAL_SETXID = 33 };
#define SIGNAL_BIT(signal) (UINT64_C(1) << ((signal) - 1))
static const uint64_t unblockable = SIGNAL_BIT(SIGNAL_KILL) | SIGNAL_BIT(SIGNAL_STOP) | SIGNAL_BIT(SIGNAL_CANCEL) | SIGNAL_BIT(SIGNAL_SETXID);
static LinuxSigaction dispositions[LINUX_SIGNAL_MAX + 1];
static _Thread_local uint64_t blocked_mask;  // Linux masks are per thread
static bool validSignal(int signal) { return signal >= 1 && signal <= LINUX_SIGNAL_MAX; }
int linuxRuntimeSigemptyset(LinuxSigset *set) {
    if (!set) return fail(LINUX_EINVAL);
    memset(set, 0, sizeof(*set));
    return 0;
}
int linuxRuntimeSigaddset(LinuxSigset *set, int signal) {
    if (!set || !validSignal(signal)) return fail(LINUX_EINVAL);
    set->words[(signal - 1) / 64] |= UINT64_C(1) << ((signal - 1) % 64);
    return 0;
}
int linuxRuntimeSigprocmask(int how, const LinuxSigset *set, LinuxSigset *old) {
    if (set && how != 0 && how != 1 && how != 2) return fail(LINUX_EINVAL);
    uint64_t current = blocked_mask, incoming = set ? set->words[0] : 0;  // read before writing old: they may alias
    if (old) old->words[0] = current;                                     // the kernel contract covers 64 signals; the rest of the set is untouched
    if (set) {
        uint64_t next = how == 0 ? current | incoming : how == 1 ? current & ~incoming : incoming;
        blocked_mask = next & ~unblockable;
    }
    return 0;
}
int linuxRuntimeSigaction(int signal, const LinuxSigaction *action, LinuxSigaction *old) {
    if (!validSignal(signal) || signal == SIGNAL_CANCEL || signal == SIGNAL_SETXID) return fail(LINUX_EINVAL);
    if (action && (signal == SIGNAL_KILL || signal == SIGNAL_STOP)) return fail(LINUX_EINVAL);
    LinuxSigaction next;
    if (action) {
        next = *action;
        next.pad = 0;
    }  // copy before writing old: they may alias
    lock();
    LinuxSigaction previous = dispositions[signal];
    if (action) dispositions[signal] = next;
    unlock();
    if (old) *old = previous;
    return 0;
}

// ---- memory remapping -------------------------------------------------------------------------
void *linuxRuntimeMremap(void *old_address, size_t old_size, size_t new_size, int flags, void *new_address) {
    (void)new_address;
    int error = LINUX_ENOSYS;
    const int known = LINUX_MREMAP_MAYMOVE | LINUX_MREMAP_FIXED | LINUX_MREMAP_DONTUNMAP;
    if ((flags & ~known) || ((flags & (LINUX_MREMAP_FIXED | LINUX_MREMAP_DONTUNMAP)) && !(flags & LINUX_MREMAP_MAYMOVE)) ||
        ((uintptr_t)old_address & (LINUX_VM_PAGE - 1)) || !new_size)
        error = LINUX_EINVAL;
    else if (flags & LINUX_MREMAP_DONTUNMAP)
        error = LINUX_EINVAL;  // unknown flag for a pre-5.7 kernel
    (void)old_size;
    fail(error);
    return LINUX_MAP_FAILED;
}

// ---- syscall ----------------------------------------------------------------------------------
#define SYSCALL_RECORDS 8u
static LinuxSyscallRecord syscall_records[SYSCALL_RECORDS];
long linuxRuntimeSyscall(long number) {
    if (number == LINUX_SYS_GETTID) return (long)(linuxPthreadSelf() & 0x7fffffff);
    lock();
    unsigned i = 0;
    while (i < SYSCALL_RECORDS && syscall_records[i].calls && syscall_records[i].number != number) ++i;
    if (i < SYSCALL_RECORDS) {
        syscall_records[i].number = number;
        ++syscall_records[i].calls;
    }
    unlock();
    *linuxAbiErrnoLocation() = LINUX_ENOSYS;
    return -1;
}
size_t linuxRuntimeUnsupportedSyscalls(LinuxSyscallRecord *out, size_t maximum) {
    size_t count = 0;
    lock();
    for (unsigned i = 0; i < SYSCALL_RECORDS && count < maximum; ++i)
        if (syscall_records[i].calls) out[count++] = syscall_records[i];
    unlock();
    return count;
}

// ---- termination ------------------------------------------------------------------------------
typedef struct Guard {
    jmp_buf context;
    struct Guard *previous;
} Guard;
static _Thread_local Guard *innermost;
static LinuxTermination termination;
bool linuxRuntimeRun(LinuxRuntimeEntry entry, void *argument, void **result) {
    Guard guard;
    guard.previous = innermost;
    if (setjmp(guard.context)) {
        innermost = guard.previous;
        return false;
    }
    innermost = &guard;
    void *value = entry(argument);
    innermost = guard.previous;
    if (result) *result = value;
    return true;
}
static void terminate(LinuxTerminationKind kind, int status, uintptr_t caller) __attribute__((noreturn));
static void terminate(LinuxTerminationKind kind, int status, uintptr_t caller) {
    LinuxTermination request = {kind, status, linuxPthreadSelf(), 1, caller};
    lock();
    if (!termination.kind)
        termination = request;
    else
        ++termination.requests;
    unlock();
    Guard *guard = innermost;
    // A thread without an armed linuxRuntimeRun context asked for termination. Unwinding is impossible and returning would run code after
    // exit/abort, so the process ends visibly.
    if (!guard) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen));
    longjmp(guard->context, 1);
}
void linuxRuntimeExit(int status) {
    linuxStdioFflush(NULL);
    terminate(LINUX_TERMINATION_EXIT, status, (uintptr_t)__builtin_return_address(0));
}
void linuxRuntimeExitImmediate(int status) { terminate(LINUX_TERMINATION_EXIT_IMMEDIATE, status, (uintptr_t)__builtin_return_address(0)); }
void linuxRuntimeAbort(void) { terminate(LINUX_TERMINATION_ABORT, 0, (uintptr_t)__builtin_return_address(0)); }
void linuxRuntimeStackCheckFail(void) { terminate(LINUX_TERMINATION_STACK_SMASH, 0, (uintptr_t)__builtin_return_address(0)); }
void linuxRuntimeUnresolvedImport(unsigned index) { terminate(LINUX_TERMINATION_UNRESOLVED_IMPORT, (int)index, 0); }
LinuxTerminationKind linuxRuntimeTerminationKindUnlocked(void) { return *(volatile LinuxTerminationKind *)&termination.kind; }
LinuxTermination linuxRuntimeTermination(void) {
    lock();
    LinuxTermination value = termination;
    unlock();
    return value;
}

// ---- dynamic lookup ---------------------------------------------------------------------------
static const char main_handle, registry_handle;  // opaque tokens: only their addresses matter
static const LinuxRuntimeExport *exports;
static size_t export_count;
void linuxRuntimeSetExports(const LinuxRuntimeExport *table, size_t count) {
    lock();
    exports = table;
    export_count = table ? count : 0;
    unlock();
}
static const LinuxDynamicLoader *dynamic_loader;
void linuxRuntimeSetDynamicLoader(const LinuxDynamicLoader *loader) {
    lock();
    dynamic_loader = loader;
    unlock();
}
static const LinuxDynamicLoader *loaderHooks(void) {
    lock();
    const LinuxDynamicLoader *loader = dynamic_loader;
    unlock();
    return loader;
}
int linuxRuntimeDlclose(void *handle) {
    const LinuxDynamicLoader *loader = loaderHooks();
    return loader && loader->close ? loader->close(handle) : 0;
}
const char *linuxRuntimeDlerror(void) {
    const LinuxDynamicLoader *loader = loaderHooks();
    return loader && loader->error ? loader->error() : NULL;
}
int linuxRuntimeDlIteratePhdr(int (*callback)(void *, size_t, void *), void *data) {
    const LinuxDynamicLoader *loader = loaderHooks();
    return loader && loader->iterate ? loader->iterate(callback, data) : 0;
}
int linuxRuntimeDladdr(const void *address, void *info) {
    (void)address;
    (void)info;
    return 0;
}
void *linuxRuntimeDlopen(const char *file, int flags) {
    const int known = LINUX_RTLD_LAZY | LINUX_RTLD_NOW | LINUX_RTLD_NOLOAD | LINUX_RTLD_DEEPBIND | LINUX_RTLD_GLOBAL | LINUX_RTLD_NODELETE;
    if (!(flags & (LINUX_RTLD_LAZY | LINUX_RTLD_NOW)) || (flags & ~known)) return NULL;
    if (!file || !*file) return (void *)&main_handle;
    const char *slash = strrchr(file, '/'), *base = slash ? slash + 1 : file;
    static const char *const provided[] = {"libc.so.6", "libz.so.1", "ld-linux-aarch64.so.1"};
    for (unsigned i = 0; i < sizeof(provided) / sizeof(provided[0]); ++i)
        if (!strcmp(base, provided[i])) return (void *)&registry_handle;
    const LinuxDynamicLoader *loader = loaderHooks();
    return loader && loader->open ? loader->open(file, flags) : NULL;
}
void *linuxRuntimeDlsym(void *handle, const char *name) {
    if (!name) return NULL;
    bool main_scope = handle == NULL || handle == (void *)&main_handle;                      // RTLD_DEFAULT or the main program
    bool next_scope = handle == (void *)(intptr_t)-1 || handle == (void *)&registry_handle;  // RTLD_NEXT or libc
    const LinuxDynamicLoader *loader = loaderHooks();
    if (!main_scope && !next_scope) return loader && loader->symbol ? loader->symbol(handle, name) : NULL;
    if (main_scope) {
        lock();
        for (size_t i = 0; i < export_count; ++i)
            if (exports[i].name && !strcmp(exports[i].name, name)) {
                void *address = (void *)exports[i].address;
                unlock();
                return address;
            }
        unlock();
        if (loader && loader->symbol) {
            void *found = loader->symbol(NULL, name);
            if (found) return found;
        }
    }
    uintptr_t address;
    return linuxAbiLookup(name, &address) ? (void *)address : NULL;
}

// ---- reset and statistics ---------------------------------------------------------------------
void linuxRuntimeReset(void) {
    linuxRuntimeInitialize();
    lock();
    storeEnvironment(default_environment, sizeof(default_environment) / sizeof(default_environment[0]));
    memcpy(limits, default_limits, sizeof(limits));
    limits_ready = true;
    memset(dispositions, 0, sizeof(dispositions));
    memset(names, 0, sizeof(names));
    memset(&termination, 0, sizeof(termination));
    memset(syscall_records, 0, sizeof(syscall_records));
    dynamic_loader = NULL;
    exports = NULL;
    export_count = 0;
    blocked_mask = 0;
    unlock();
}
