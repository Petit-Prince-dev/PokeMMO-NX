#pragma once
#include "linux_abi.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

// Adapters for the C library surface the client's native libraries (LWJGL, libgdx, FreeType, the C++ runtime)
// use on top of what the client itself imports: fortified printf family, C-locale and wide-character helpers,
// pthread once/rwlock, time breakdown, file-system aliases. Everything speaks the glibc AArch64 ABI.

// ---- printf family (fortified variants; the flag and object-size arguments are ignored) -------------------------
int linuxExtraSnprintfChk(char *buffer, size_t size, int flag, size_t object_size, const char *format, ...);
int linuxExtraVsnprintfChk(char *buffer, size_t size, int flag, size_t object_size, const char *format, va_list arguments);
int linuxExtraSprintf(char *buffer, const char *format, ...);
int linuxExtraSprintfChk(char *buffer, int flag, size_t object_size, const char *format, ...);
int linuxExtraFprintfChk(void *stream, int flag, const char *format, ...);
int linuxExtraPrintf(const char *format, ...);
int linuxExtraPrintfChk(int flag, const char *format, ...);
int linuxExtraPuts(const char *text);
void *linuxExtraMemcpyChk(void *target, const void *source, size_t bytes, size_t object_size);
void *linuxExtraMemmoveChk(void *target, const void *source, size_t bytes, size_t object_size);
void *linuxExtraMemsetChk(void *target, int value, size_t bytes, size_t object_size);
char *linuxExtraStrcpyChk(char *target, const char *source, size_t object_size);
char *linuxExtraStrcatChk(char *target, const char *source, size_t object_size);
void linuxExtraAssertFail(const char *expression, const char *file, unsigned line, const char *function) __attribute__((noreturn));

// ---- stdio on managed streams ------------------------------------------------------------------------------------
int linuxExtraFgetc(void *stream);
int linuxExtraFputc(int character, void *stream);
int linuxExtraUngetc(int character, void *stream);  // not supported: EOF
int linuxExtraSetvbuf(void *stream, char *buffer, int mode, size_t size);
int linuxExtraFseek(void *stream, long offset, int whence);
long linuxExtraFtell(void *stream);
int64_t linuxExtraGetdelim(char **line, size_t *capacity, int delimiter, void *stream);
int64_t linuxExtraGetline(char **line, size_t *capacity, void *stream);
int linuxExtraFscanf(void *stream, const char *format, ...);  // not supported: EOF
uint32_t linuxExtraWideEof(void);                             // getwc/putwc/ungetwc: WEOF

// ---- C locale (the only one) and wide characters (ASCII range) ---------------------------------------------------
void *linuxExtraNewlocale(int mask, const char *name, void *base);
void *linuxExtraDuplocale(void *locale);
void linuxExtraFreelocale(void *locale);
void *linuxExtraUselocale(void *locale);
size_t linuxExtraMbCurMax(void);
char *linuxExtraNlLanginfo(int item);
char *linuxExtraNlLanginfoL(int item, void *locale);
int linuxExtraStrcollL(const char *a, const char *b, void *locale);
size_t linuxExtraStrxfrmL(char *target, const char *source, size_t bytes, void *locale);
int linuxExtraWcscollL(const int32_t *a, const int32_t *b, void *locale);
size_t linuxExtraWcsxfrmL(int32_t *target, const int32_t *source, size_t count, void *locale);
double linuxExtraStrtodL(const char *text, char **end, void *locale);
float linuxExtraStrtofL(const char *text, char **end, void *locale);
long double linuxExtraStrtoldL(const char *text, char **end, void *locale);
uint32_t linuxExtraBtowc(int byte);
int linuxExtraWctob(uint32_t wide);
size_t linuxExtraMbrtowc(int32_t *wide, const char *bytes, size_t count, void *state);
size_t linuxExtraWcrtomb(char *bytes, int32_t wide, void *state);
size_t linuxExtraMbsrtowcs(int32_t *target, const char **source, size_t count, void *state);
size_t linuxExtraMbsnrtowcs(int32_t *target, const char **source, size_t source_bytes, size_t count, void *state);
size_t linuxExtraWcsnrtombs(char *target, const int32_t **source, size_t source_count, size_t bytes, void *state);
size_t linuxExtraWcslen(const int32_t *text);
int linuxExtraWcscmp(const int32_t *a, const int32_t *b);
int32_t *linuxExtraWmemchr(const int32_t *text, int32_t c, size_t count);
int linuxExtraWmemcmp(const int32_t *a, const int32_t *b, size_t count);
int32_t *linuxExtraWmemcpy(int32_t *target, const int32_t *source, size_t count);
int32_t *linuxExtraWmemmove(int32_t *target, const int32_t *source, size_t count);
int32_t *linuxExtraWmemset(int32_t *target, int32_t c, size_t count);
extern void *linuxExtraStdin;                   // opaque stdin token (never readable)
extern unsigned char linuxExtraSingleThreaded;  // __libc_single_threaded: always 0
unsigned long linuxExtraWctypeL(const char *name, void *locale);
unsigned long linuxExtraWctype(const char *name);
int linuxExtraIswctypeL(uint32_t wide, unsigned long type, void *locale);
int linuxExtraIswctype(uint32_t wide, unsigned long type);
uint32_t linuxExtraTowlowerL(uint32_t wide, void *locale);
uint32_t linuxExtraTowupperL(uint32_t wide, void *locale);
int linuxExtraTolower(int c);
int linuxExtraToupper(int c);

// ---- time ----------------------------------------------------------------------------------------------------------
// glibc struct tm (56 bytes): nine ints, padding, tm_gmtoff and tm_zone. The zone is UTC.
typedef struct {
    int32_t second, minute, hour, day, month, year, weekday, yearday, daylight;
    int32_t padding;
    int64_t gmtoff;
    const char *zone;
} LinuxTm;
LinuxTm *linuxExtraGmtimeR(const int64_t *time, LinuxTm *result);
LinuxTm *linuxExtraLocaltimeR(const int64_t *time, LinuxTm *result);
size_t linuxExtraStrftime(char *buffer, size_t size, const char *format, const LinuxTm *tm);
size_t linuxExtraStrftimeL(char *buffer, size_t size, const char *format, const LinuxTm *tm, void *locale);
int64_t linuxExtraTimegm(LinuxTm *tm);
void linuxExtraTzset(void);

// ---- threads -----------------------------------------------------------------------------------------------------------
int linuxExtraPthreadOnce(void *once, void (*routine)(void));
int linuxExtraRwlockInit(void *lock, const void *attributes);
int linuxExtraRwlockDestroy(void *lock);
int linuxExtraRwlockRdlock(void *lock);
int linuxExtraRwlockTryRdlock(void *lock);
int linuxExtraRwlockWrlock(void *lock);
int linuxExtraRwlockTryWrlock(void *lock);
int linuxExtraRwlockUnlock(void *lock);
int linuxExtraMutexattrInit(void *attributes);
int linuxExtraMutexattrDestroy(void *attributes);
int linuxExtraMutexattrSettype(void *attributes, int type);
int linuxExtraPthreadSigmask(int how, const void *set, void *old);
int linuxExtraPthreadGetnameNp(uint64_t thread, char *buffer, size_t bytes);
int linuxExtraSchedGetscheduler(int pid);
int linuxExtraSchedGetparam(int pid, void *parameters);
int linuxExtraSchedSetscheduler(int pid, int policy, const void *parameters);
int linuxExtraSetpriority(int which, unsigned who, int priority);
int linuxExtraSchedGetcpu(void);
int linuxExtraSchedSetaffinity(int pid, size_t bytes, const void *mask);
int linuxExtraGetNprocs(void);

// ---- identity, randomness, process ----------------------------------------------------------------------------------
uint32_t linuxExtraGetid(void);  // geteuid, getgid, getegid: the single virtual user
int linuxExtraGetppid(void);
unsigned linuxExtraUmask(unsigned mask);
int linuxExtraGetentropy(void *buffer, size_t bytes);
int64_t linuxExtraGetrandom(void *buffer, size_t bytes, unsigned flags);
uint32_t linuxExtraArc4random(void);
char *linuxExtraSecureGetenv(const char *name);
int linuxExtraCxaAtexit(void (*function)(void *), void *argument, void *dso);
int linuxExtraCxaThreadAtexit(void (*function)(void *), void *argument, void *dso);
int linuxExtraRegisterAtfork(void (*prepare)(void), void (*parent)(void), void (*child)(void), void *dso);
int64_t linuxExtraDlFindObject(void *address, void *result);
void *linuxExtraAlignedAlloc(size_t alignment, size_t bytes);
char *linuxExtraStrerror(int error);
int64_t linuxExtraStrtoimax(const char *text, char **end, int base);
void linuxExtraSincos(double x, double *sine, double *cosine);

// ---- file system aliases and explicit refusals --------------------------------------------------------------------------
int linuxExtraStat(const char *path, void *output);
int linuxExtraLstat(const char *path, void *output);
int linuxExtraFstat(int fd, void *output);
int linuxExtraOpenat(int directory, const char *path, int flags, ...);
int linuxExtraUnlinkat(int directory, const char *path, int flags);
int linuxExtraZero(void);  // fchmod, fchmodat, utimensat, futimens ...: no permission or time model, success
int linuxExtraTruncate(const char *path, int64_t size);
int linuxExtraRefuseNosys(void);    // symlink, link, statfs, statvfs, memfd_create ...: -1 with ENOSYS
int linuxExtraRefuseNoentFd(void);  // dirfd, fdopendir: -1 with ENOSYS
