#include "linux_libc_extra.h"
#include "linux_directories.h"
#include "linux_files.h"
#include "linux_format.h"
#include "linux_runtime.h"
#include "linux_stdio.h"
#include "linux_threads.h"
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <switch.h>

void *linuxExtraStdin;
unsigned char linuxExtraSingleThreaded;
static int fail(int error) {
    *linuxAbiErrnoLocation() = error;
    return -1;
}

// ---- printf family ------------------------------------------------------------------------------------------
int linuxExtraVsnprintfChk(char *buffer, size_t size, int flag, size_t object_size, const char *format, va_list arguments) {
    (void)flag;
    (void)object_size;
    return linuxFormatVsnprintf(buffer, size, format, arguments);
}
int linuxExtraSnprintfChk(char *buffer, size_t size, int flag, size_t object_size, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = linuxExtraVsnprintfChk(buffer, size, flag, object_size, format, arguments);
    va_end(arguments);
    return result;
}
int linuxExtraSprintf(char *buffer, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = linuxFormatVsnprintf(buffer, LINUX_FORMAT_MAX_OUTPUT, format, arguments);
    va_end(arguments);
    return result;
}
int linuxExtraSprintfChk(char *buffer, int flag, size_t object_size, const char *format, ...) {
    (void)flag;
    (void)object_size;
    va_list arguments;
    va_start(arguments, format);
    int result = linuxFormatVsnprintf(buffer, LINUX_FORMAT_MAX_OUTPUT, format, arguments);
    va_end(arguments);
    return result;
}
int linuxExtraFprintfChk(void *stream, int flag, const char *format, ...) {
    (void)flag;
    va_list arguments;
    va_start(arguments, format);
    int result = linuxFormatVfprintf(stream, format, arguments);
    va_end(arguments);
    return result;
}
int linuxExtraPrintf(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = linuxFormatVfprintf(linuxStdioStdout, format, arguments);
    va_end(arguments);
    return result;
}
int linuxExtraPrintfChk(int flag, const char *format, ...) {
    (void)flag;
    va_list arguments;
    va_start(arguments, format);
    int result = linuxFormatVfprintf(linuxStdioStdout, format, arguments);
    va_end(arguments);
    return result;
}
int linuxExtraPuts(const char *text) {
    if (!text) return fail(LINUX_EFAULT);
    if (linuxStdioFputs(text, linuxStdioStdout) < 0 || linuxStdioFwrite("\n", 1, 1, linuxStdioStdout) != 1) return -1;
    return 1;
}
void *linuxExtraMemcpyChk(void *target, const void *source, size_t bytes, size_t object_size) {
    (void)object_size;
    return memcpy(target, source, bytes);
}
void *linuxExtraMemmoveChk(void *target, const void *source, size_t bytes, size_t object_size) {
    (void)object_size;
    return memmove(target, source, bytes);
}
void *linuxExtraMemsetChk(void *target, int value, size_t bytes, size_t object_size) {
    (void)object_size;
    return memset(target, value, bytes);
}
char *linuxExtraStrcpyChk(char *target, const char *source, size_t object_size) {
    (void)object_size;
    return strcpy(target, source);
}
char *linuxExtraStrcatChk(char *target, const char *source, size_t object_size) {
    (void)object_size;
    return strcat(target, source);
}
void linuxExtraAssertFail(const char *expression, const char *file, unsigned line, const char *function) {
    linuxFormatFprintf(linuxStdioStderr, "assertion failed: %s (%s:%u %s)\n", expression ? expression : "?", file ? file : "?", line,
                       function ? function : "?");
    linuxRuntimeAbort();
}

// ---- stdio on managed streams ----------------------------------------------------------------------------------
int linuxExtraFgetc(void *stream) {
    unsigned char value;
    return linuxStdioFread(&value, 1, 1, stream) == 1 ? value : -1;
}
int linuxExtraFputc(int character, void *stream) {
    unsigned char value = (unsigned char)character;
    return linuxStdioFwrite(&value, 1, 1, stream) == 1 ? value : -1;
}
int linuxExtraUngetc(int character, void *stream) {
    (void)character;
    (void)stream;
    return -1;
}
int linuxExtraSetvbuf(void *stream, char *buffer, int mode, size_t size) {
    (void)stream;
    (void)buffer;
    (void)mode;
    (void)size;
    return 0;
}
int linuxExtraFseek(void *stream, long offset, int whence) { return linuxStdioFseeko(stream, offset, whence); }
long linuxExtraFtell(void *stream) { return (long)linuxStdioFtello(stream); }
int64_t linuxExtraGetdelim(char **line, size_t *capacity, int delimiter, void *stream) {
    if (!line || !capacity || !stream) return fail(LINUX_EINVAL);
    size_t length = 0;
    for (;;) {
        int c = linuxExtraFgetc(stream);
        if (c < 0) break;
        if (length + 2 > *capacity || !*line) {
            size_t wanted = *capacity ? *capacity * 2 : 128;
            char *grown = linuxAbiRealloc(*line, wanted);
            if (!grown) return -1;
            *line = grown;
            *capacity = wanted;
        }
        (*line)[length++] = (char)c;
        if (c == (delimiter & 0xff)) break;
    }
    if (!length) return -1;
    (*line)[length] = 0;
    return (int64_t)length;
}
int64_t linuxExtraGetline(char **line, size_t *capacity, void *stream) { return linuxExtraGetdelim(line, capacity, '\n', stream); }
int linuxExtraFscanf(void *stream, const char *format, ...) {
    (void)stream;
    (void)format;
    return -1;
}
uint32_t linuxExtraWideEof(void) { return 0xffffffffu; }

// ---- C locale and wide characters -------------------------------------------------------------------------------------
static char c_locale[64];
void *linuxExtraNewlocale(int mask, const char *name, void *base) {
    (void)mask;
    (void)base;
    if (name && *name && strcmp(name, "C") && strcmp(name, "POSIX")) {
        *linuxAbiErrnoLocation() = LINUX_ENOENT;
        return NULL;
    }
    return c_locale;
}
void *linuxExtraDuplocale(void *locale) {
    (void)locale;
    return c_locale;
}
void linuxExtraFreelocale(void *locale) { (void)locale; }
void *linuxExtraUselocale(void *locale) {
    (void)locale;
    return c_locale;
}
size_t linuxExtraMbCurMax(void) { return 1; }
static char *langinfo(int item) {
    switch (item) {
        case 14: return "ANSI_X3.4-1968";  // CODESET
        case 0x10000: return ".";          // RADIXCHAR
        default: return "";
    }
}
char *linuxExtraNlLanginfo(int item) { return langinfo(item); }
char *linuxExtraNlLanginfoL(int item, void *locale) {
    (void)locale;
    return langinfo(item);
}
int linuxExtraStrcollL(const char *a, const char *b, void *locale) {
    (void)locale;
    return strcmp(a, b);
}
size_t linuxExtraStrxfrmL(char *target, const char *source, size_t bytes, void *locale) {
    (void)locale;
    size_t length = strlen(source);
    if (bytes) {
        size_t copy = length < bytes - 1 ? length : bytes - 1;
        memcpy(target, source, copy);
        target[copy] = 0;
    }
    return length;
}
size_t linuxExtraWcslen(const int32_t *text) {
    size_t n = 0;
    while (text[n]) ++n;
    return n;
}
int linuxExtraWcscmp(const int32_t *a, const int32_t *b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (*a > *b) - (*a < *b);
}
int32_t *linuxExtraWmemchr(const int32_t *text, int32_t c, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (text[i] == c) return (int32_t *)(text + i);
    return NULL;
}
int linuxExtraWmemcmp(const int32_t *a, const int32_t *b, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
int32_t *linuxExtraWmemcpy(int32_t *target, const int32_t *source, size_t count) { return memcpy(target, source, count * 4); }
int32_t *linuxExtraWmemmove(int32_t *target, const int32_t *source, size_t count) { return memmove(target, source, count * 4); }
int32_t *linuxExtraWmemset(int32_t *target, int32_t c, size_t count) {
    for (size_t i = 0; i < count; ++i) target[i] = c;
    return target;
}
int linuxExtraWcscollL(const int32_t *a, const int32_t *b, void *locale) {
    (void)locale;
    return linuxExtraWcscmp(a, b);
}
size_t linuxExtraWcsxfrmL(int32_t *target, const int32_t *source, size_t count, void *locale) {
    (void)locale;
    size_t length = linuxExtraWcslen(source);
    if (count) {
        size_t copy = length < count - 1 ? length : count - 1;
        memcpy(target, source, copy * 4);
        target[copy] = 0;
    }
    return length;
}
double linuxExtraStrtodL(const char *text, char **end, void *locale) {
    (void)locale;
    return strtod(text, end);
}
float linuxExtraStrtofL(const char *text, char **end, void *locale) {
    (void)locale;
    return strtof(text, end);
}
long double linuxExtraStrtoldL(const char *text, char **end, void *locale) {
    (void)locale;
    return strtold(text, end);
}
uint32_t linuxExtraBtowc(int byte) { return byte >= 0 && byte < 128 ? (uint32_t)byte : 0xffffffffu; }
int linuxExtraWctob(uint32_t wide) { return wide < 128 ? (int)wide : -1; }
size_t linuxExtraMbrtowc(int32_t *wide, const char *bytes, size_t count, void *state) {
    (void)state;
    if (!bytes) return 0;
    if (!count) return (size_t)-2;
    unsigned char value = (unsigned char)bytes[0];
    if (value >= 128) {
        *linuxAbiErrnoLocation() = LINUX_EILSEQ;
        return (size_t)-1;
    }
    if (wide) *wide = value;
    return value ? 1 : 0;
}
size_t linuxExtraWcrtomb(char *bytes, int32_t wide, void *state) {
    (void)state;
    if (!bytes) return 1;
    if (wide < 0 || wide >= 128) {
        *linuxAbiErrnoLocation() = LINUX_EILSEQ;
        return (size_t)-1;
    }
    bytes[0] = (char)wide;
    return 1;
}
size_t linuxExtraMbsrtowcs(int32_t *target, const char **source, size_t count, void *state) {
    (void)state;
    const unsigned char *text = (const unsigned char *)*source;
    size_t i = 0;
    for (;; ++i) {
        if (target && i == count) {
            *source = (const char *)(text + i);
            return i;
        }
        if (text[i] >= 128) {
            *linuxAbiErrnoLocation() = LINUX_EILSEQ;
            if (target) *source = (const char *)(text + i);
            return (size_t)-1;
        }
        if (target) target[i] = text[i];
        if (!text[i]) {
            if (target) *source = NULL;
            return i;
        }
    }
}
size_t linuxExtraMbsnrtowcs(int32_t *target, const char **source, size_t source_bytes, size_t count, void *state) {
    (void)state;
    const unsigned char *text = (const unsigned char *)*source;
    size_t i = 0;
    for (; i < source_bytes; ++i) {
        if (target && i == count) break;
        if (text[i] >= 128) {
            *linuxAbiErrnoLocation() = LINUX_EILSEQ;
            if (target) *source = (const char *)(text + i);
            return (size_t)-1;
        }
        if (target) target[i] = text[i];
        if (!text[i]) {
            if (target) *source = NULL;
            return i;
        }
    }
    if (target) *source = (const char *)(text + i);
    return i;
}
size_t linuxExtraWcsnrtombs(char *target, const int32_t **source, size_t source_count, size_t bytes, void *state) {
    (void)state;
    const int32_t *text = *source;
    size_t i = 0;
    for (; i < source_count; ++i) {
        if (target && i == bytes) break;
        if (text[i] < 0 || text[i] >= 128) {
            *linuxAbiErrnoLocation() = LINUX_EILSEQ;
            if (target) *source = text + i;
            return (size_t)-1;
        }
        if (target) target[i] = (char)text[i];
        if (!text[i]) {
            if (target) *source = NULL;
            return i;
        }
    }
    if (target) *source = text + i;
    return i;
}
static const char *const wide_classes[] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit"};
unsigned long linuxExtraWctype(const char *name) {
    for (unsigned i = 0; i < sizeof(wide_classes) / sizeof(wide_classes[0]); ++i)
        if (!strcmp(name, wide_classes[i])) return i + 1;
    return 0;
}
unsigned long linuxExtraWctypeL(const char *name, void *locale) {
    (void)locale;
    return linuxExtraWctype(name);
}
int linuxExtraIswctype(uint32_t wide, unsigned long type) {
    if (wide >= 128 || type < 1 || type > sizeof(wide_classes) / sizeof(wide_classes[0])) return 0;
    int c = (int)wide;
    int upper = c >= 'A' && c <= 'Z', lower = c >= 'a' && c <= 'z', digit = c >= '0' && c <= '9';
    switch (type) {
        case 1: return upper || lower || digit;
        case 2: return upper || lower;
        case 3: return c == ' ' || c == '\t';
        case 4: return c < 32 || c == 127;
        case 5: return digit;
        case 6: return c > 32 && c < 127;
        case 7: return lower;
        case 8: return c >= 32 && c < 127;
        case 9: return c > 32 && c < 127 && !(upper || lower || digit);
        case 10: return c == ' ' || (c >= 9 && c <= 13);
        case 11: return upper;
        default: return digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
}
int linuxExtraIswctypeL(uint32_t wide, unsigned long type, void *locale) {
    (void)locale;
    return linuxExtraIswctype(wide, type);
}
uint32_t linuxExtraTowlowerL(uint32_t wide, void *locale) {
    (void)locale;
    return wide >= 'A' && wide <= 'Z' ? wide + 32 : wide;
}
uint32_t linuxExtraTowupperL(uint32_t wide, void *locale) {
    (void)locale;
    return wide >= 'a' && wide <= 'z' ? wide - 32 : wide;
}
int linuxExtraTolower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int linuxExtraToupper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

// ---- time (UTC) ---------------------------------------------------------------------------------------------------------------
// Days since 1970-01-01 <-> civil date (proleptic Gregorian), after Howard Hinnant's algorithms.
static int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400), doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
LinuxTm *linuxExtraGmtimeR(const int64_t *time, LinuxTm *result) {
    if (!time || !result) {
        *linuxAbiErrnoLocation() = LINUX_EFAULT;
        return NULL;
    }
    int64_t t = *time, days = t / 86400, rest = t % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    if (y - 1900 > INT_MAX || y - 1900 < INT_MIN) {
        *linuxAbiErrnoLocation() = LINUX_EOVERFLOW;
        return NULL;
    }
    memset(result, 0, sizeof(*result));
    result->second = (int32_t)(rest % 60);
    result->minute = (int32_t)(rest / 60 % 60);
    result->hour = (int32_t)(rest / 3600);
    result->day = (int32_t)d;
    result->month = (int32_t)m - 1;
    result->year = (int32_t)(y - 1900);
    int64_t weekday = (days + 4) % 7;
    result->weekday = (int32_t)(weekday < 0 ? weekday + 7 : weekday);
    result->yearday = (int32_t)(days - daysFromCivil(y, 1, 1));
    result->zone = "UTC";
    return result;
}
LinuxTm *linuxExtraLocaltimeR(const int64_t *time, LinuxTm *result) { return linuxExtraGmtimeR(time, result); }
int64_t linuxExtraTimegm(LinuxTm *tm) {
    int64_t year = tm->year + 1900 + tm->month / 12;
    int month = tm->month % 12;
    if (month < 0) {
        month += 12;
        --year;
    }
    return (daysFromCivil(year, (unsigned)month + 1, 1) + tm->day - 1) * 86400 + tm->hour * 3600 + tm->minute * 60 + tm->second;
}
void linuxExtraTzset(void) {}
size_t linuxExtraStrftime(char *buffer, size_t size, const char *format, const LinuxTm *tm) {
    struct tm local;
    memset(&local, 0, sizeof(local));
    local.tm_sec = tm->second;
    local.tm_min = tm->minute;
    local.tm_hour = tm->hour;
    local.tm_mday = tm->day;
    local.tm_mon = tm->month;
    local.tm_year = tm->year;
    local.tm_wday = tm->weekday;
    local.tm_yday = tm->yearday;
    local.tm_isdst = tm->daylight;
    return strftime(buffer, size, format, &local);
}
size_t linuxExtraStrftimeL(char *buffer, size_t size, const char *format, const LinuxTm *tm, void *locale) {
    (void)locale;
    return linuxExtraStrftime(buffer, size, format, tm);
}

// ---- threads --------------------------------------------------------------------------------------------------------------------
int linuxExtraPthreadOnce(void *once, void (*routine)(void)) {
    _Atomic int *state = once;
    int expected = 0;
    if (!state || !routine) return LINUX_EINVAL;
    if (atomic_compare_exchange_strong(state, &expected, 1)) {
        routine();
        atomic_store(state, 2);
        return 0;
    }
    while (atomic_load(state) == 1) linuxThreadYield();
    return 0;
}
// pthread_rwlock_t is 56 bytes; the first word holds the state: readers in the low bits, writer flag on top.
#define WRITER 0x80000000u
int linuxExtraRwlockInit(void *lock, const void *attributes) {
    (void)attributes;
    if (!lock) return LINUX_EINVAL;
    memset(lock, 0, 56);
    return 0;
}
int linuxExtraRwlockDestroy(void *lock) {
    if (!lock) return LINUX_EINVAL;
    memset(lock, 0, 56);
    return 0;
}
int linuxExtraRwlockTryRdlock(void *lock) {
    _Atomic unsigned *state = lock;
    unsigned current = atomic_load(state);
    while (!(current & WRITER))
        if (atomic_compare_exchange_weak(state, &current, current + 1)) return 0;
    return LINUX_EBUSY;
}
int linuxExtraRwlockRdlock(void *lock) {
    while (linuxExtraRwlockTryRdlock(lock)) linuxThreadYield();
    return 0;
}
int linuxExtraRwlockTryWrlock(void *lock) {
    _Atomic unsigned *state = lock;
    unsigned expected = 0;
    return atomic_compare_exchange_strong(state, &expected, WRITER) ? 0 : LINUX_EBUSY;
}
int linuxExtraRwlockWrlock(void *lock) {
    while (linuxExtraRwlockTryWrlock(lock)) linuxThreadYield();
    return 0;
}
int linuxExtraRwlockUnlock(void *lock) {
    _Atomic unsigned *state = lock;
    unsigned current = atomic_load(state);
    for (;;) {
        unsigned next = (current & WRITER) ? 0 : (current ? current - 1 : 0);
        if (atomic_compare_exchange_weak(state, &current, next)) return 0;
    }
}
int linuxExtraMutexattrInit(void *attributes) {
    if (!attributes) return LINUX_EINVAL;
    memset(attributes, 0, 8);
    return 0;
}
int linuxExtraMutexattrDestroy(void *attributes) { return attributes ? 0 : LINUX_EINVAL; }
int linuxExtraMutexattrSettype(void *attributes, int type) {
    if (!attributes || type < 0 || type > 2) return LINUX_EINVAL;
    memcpy(attributes, &(uint32_t){(uint32_t)type}, 4);
    return 0;
}
int linuxExtraPthreadSigmask(int how, const void *set, void *old) { return linuxRuntimeSigprocmask(how, set, old) ? *linuxAbiErrnoLocation() : 0; }
int linuxExtraPthreadGetnameNp(uint64_t thread, char *buffer, size_t bytes) {
    char name[LINUX_THREAD_NAME_MAX + 1];
    if (!linuxRuntimeGetThreadName(thread, name)) return LINUX_ESRCH;
    size_t length = strlen(name);
    if (bytes < length + 1) return LINUX_ERANGE;
    memcpy(buffer, name, length + 1);
    return 0;
}
int linuxExtraSchedGetscheduler(int pid) {
    (void)pid;
    return 0;
}
int linuxExtraSchedGetparam(int pid, void *parameters) {
    (void)pid;
    if (parameters) memset(parameters, 0, 4);
    return 0;
}
int linuxExtraSchedSetscheduler(int pid, int policy, const void *parameters) {
    (void)pid;
    (void)policy;
    (void)parameters;
    return fail(LINUX_EPERM);
}
int linuxExtraSetpriority(int which, unsigned who, int priority) {
    (void)which;
    (void)who;
    (void)priority;
    return 0;
}
int linuxExtraSchedGetcpu(void) { return 0; }
int linuxExtraSchedSetaffinity(int pid, size_t bytes, const void *mask) {
    (void)pid;
    (void)bytes;
    (void)mask;
    return 0;
}
int linuxExtraGetNprocs(void) {
    int64_t count = linuxAbiSysconf(84);
    return count > 0 ? (int)count : 1;
}

// ---- identity, randomness, process ------------------------------------------------------------------------------------
uint32_t linuxExtraGetid(void) { return LINUX_UID; }
int linuxExtraGetppid(void) { return 1; }
unsigned linuxExtraUmask(unsigned mask) {
    (void)mask;
    return 022;
}
int linuxExtraGetentropy(void *buffer, size_t bytes) {
    if (bytes > 256) return fail(LINUX_EIO);
    randomGet(buffer, bytes);
    return 0;
}
int64_t linuxExtraGetrandom(void *buffer, size_t bytes, unsigned flags) {
    (void)flags;
    if (bytes > 33554431) bytes = 33554431;
    randomGet(buffer, bytes);
    return (int64_t)bytes;
}
uint32_t linuxExtraArc4random(void) {
    uint32_t value = 0;
    randomGet(&value, sizeof(value));
    return value;
}
char *linuxExtraSecureGetenv(const char *name) { return linuxRuntimeGetenv(name); }
// Registered termination functions are accepted but never run: a process ends by termination, not by returning from main.
int linuxExtraCxaAtexit(void (*function)(void *), void *argument, void *dso) {
    (void)function;
    (void)argument;
    (void)dso;
    return 0;
}
int linuxExtraCxaThreadAtexit(void (*function)(void *), void *argument, void *dso) {
    (void)function;
    (void)argument;
    (void)dso;
    return 0;
}
int linuxExtraRegisterAtfork(void (*prepare)(void), void (*parent)(void), void (*child)(void), void *dso) {
    (void)prepare;
    (void)parent;
    (void)child;
    (void)dso;
    return 0;
}
int64_t linuxExtraDlFindObject(void *address, void *result) {
    (void)address;
    (void)result;
    return -1;
}  // the unwinder then falls back to dl_iterate_phdr
void *linuxExtraAlignedAlloc(size_t alignment, size_t bytes) {
    if (!alignment || (alignment & (alignment - 1)) || bytes % alignment) {
        *linuxAbiErrnoLocation() = LINUX_EINVAL;
        return NULL;
    }
    return aligned_alloc(alignment, bytes ? bytes : alignment);
}
char *linuxExtraStrerror(int error) {
    static _Thread_local char buffer[96];
    return linuxRuntimeStrerrorR(error, buffer, sizeof(buffer));
}
int64_t linuxExtraStrtoimax(const char *text, char **end, int base) { return linuxAbiStrtol(text, end, base); }
void linuxExtraSincos(double x, double *sine, double *cosine) {
    *sine = sin(x);
    *cosine = cos(x);
}

// ---- file system ------------------------------------------------------------------------------------------------------------------
int linuxExtraStat(const char *path, void *output) { return linuxAbiXstat(0, path, output); }
int linuxExtraLstat(const char *path, void *output) { return linuxAbiLxstat(0, path, output); }
int linuxExtraFstat(int fd, void *output) { return linuxAbiFxstat(0, fd, output); }
#define AT_FDCWD (-100)
static bool relativeToCwd(int directory, const char *path) { return directory == AT_FDCWD || (path && path[0] == '/'); }
int linuxExtraOpenat(int directory, const char *path, int flags, ...) {
    if (!relativeToCwd(directory, path)) return fail(LINUX_ENOSYS);
    unsigned mode = 0;
    if (flags & LINUX_O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, unsigned);
        va_end(arguments);
    }
    return linuxAbiOpen(path, flags, mode);
}
int linuxExtraUnlinkat(int directory, const char *path, int flags) {
    if (!relativeToCwd(directory, path)) return fail(LINUX_ENOSYS);
    return (flags & 0x200) ? linuxAbiRmdir(path) : linuxAbiUnlink(path);
}
int linuxExtraZero(void) { return 0; }
int linuxExtraTruncate(const char *path, int64_t size) {
    int fd = linuxAbiOpen(path, LINUX_O_WRONLY);
    if (fd < 0) return -1;
    int result = linuxAbiFtruncate(fd, size);
    int saved = *linuxAbiErrnoLocation();
    linuxAbiClose(fd);
    if (result) *linuxAbiErrnoLocation() = saved;
    return result;
}
int linuxExtraRefuseNosys(void) { return fail(LINUX_ENOSYS); }
int linuxExtraRefuseNoentFd(void) { return fail(LINUX_ENOSYS); }
