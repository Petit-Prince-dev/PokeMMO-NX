#include "linux_format.h"
#include "linux_stdio.h"
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
_Static_assert(sizeof(va_list) == 32 && sizeof(long) == 8, "AAPCS64 LP64 varargs required");
static int fail(int value) {
    *linuxAbiErrnoLocation() = value;
    return -1;
}
static bool digit(char c) { return c >= '0' && c <= '9'; }
static bool number(const char **cursor) {
    unsigned value = 0;
    const char *p = *cursor;
    while (digit(*p)) {
        unsigned d = (unsigned)(*p++ - '0');
        if (value > ((unsigned)INT_MAX - d) / 10) return false;
        value = value * 10 + d;
    }
    *cursor = p;
    return true;
}
// Reject unsupported extensions before consuming arguments or emitting bytes.
// %n, positional arguments, wide characters and binary128 are not adapted.
static bool validate(const char *format) {
    for (const char *p = format; *p;) {
        if (*p++ != '%') continue;
        if (*p == '%') {
            ++p;
            continue;
        }
        while (*p && strchr("-+ #0", *p)) ++p;
        if (*p == '*')
            ++p;
        else if (!number(&p))
            return false;
        if (*p == '.') {
            ++p;
            if (*p == '*')
                ++p;
            else if (!number(&p))
                return false;
        }
        unsigned length = 0;
        if (*p == 'h') {
            length = 1;
            ++p;
            if (*p == 'h') {
                ++p;
                length = 2;
            }
        } else if (*p == 'l') {
            length = 3;
            ++p;
            if (*p == 'l') {
                ++p;
                length = 4;
            }
        } else if (*p == 'j' || *p == 'z' || *p == 't') {
            length = 5;
            ++p;
        }
        if (!*p) return false;
        if (strchr("diouxX", *p)) {
            ++p;
            continue;
        }
        if (strchr("aAeEfFgG", *p) && (!length || length == 3)) {
            ++p;
            continue;
        }
        if (strchr("csp", *p) && !length) {
            ++p;
            continue;
        }
        return false;
    }
    return true;
}
static int nativeError(int e) {
    if (e == ENOMEM) return LINUX_ENOMEM;
    if (e == EILSEQ) return LINUX_EILSEQ;
    if (e == EOVERFLOW) return LINUX_EOVERFLOW;
    if (e == EINVAL) return LINUX_EINVAL;
    return LINUX_EIO;
}
static int render(char *out, size_t bytes, const char *format, va_list args) {
    int saved = errno;
    errno = 0;
    va_list working;
    va_copy(working, args);
    int result = vsnprintf(out, bytes, format, working);
    va_end(working);
    int e = errno;
    errno = saved;
    return result < 0 ? fail(nativeError(e)) : result;
}
int linuxFormatVsnprintf(char *out, size_t bytes, const char *format, va_list args) {
    if ((bytes && !out) || !format) return fail(LINUX_EFAULT);
    if (!validate(format)) {
        if (bytes) out[0] = 0;
        return fail(LINUX_ENOTSUP);
    }
    return render(out, bytes, format, args);
}
int linuxFormatSnprintf(char *out, size_t bytes, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = linuxFormatVsnprintf(out, bytes, format, args);
    va_end(args);
    return result;
}
int linuxFormatVfprintf(void *stream, const char *format, va_list args) {
    if (!format) return fail(LINUX_EFAULT);
    if (!validate(format)) return fail(LINUX_ENOTSUP);
    int count = render(NULL, 0, format, args);
    if (count < 0) return -1;
    if ((unsigned)count > LINUX_FORMAT_MAX_OUTPUT) return fail(LINUX_EOVERFLOW);
    char small[256];
    char *text = (unsigned)count < sizeof(small) ? small : linuxAbiMalloc((size_t)count + 1);
    if (!text) return fail(LINUX_ENOMEM);
    int result = render(text, (size_t)count + 1, format, args);
    // Invalid tokens and read-only streams must also fail for empty output.
    if (result >= 0 && result != count) result = fail(LINUX_EIO);
    if (result >= 0) result = linuxStdioWriteRecord(stream, text, (size_t)count) == count ? count : fail(*linuxAbiErrnoLocation());
    if (text != small) linuxAbiFree(text);
    return result;
}
int linuxFormatFprintf(void *stream, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = linuxFormatVfprintf(stream, format, args);
    va_end(args);
    return result;
}
