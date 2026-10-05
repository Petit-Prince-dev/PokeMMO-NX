#include "linux_stdio.h"
#include "linux_files.h"
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#include <switch.h>
static Mutex registry;
static void lockStreams(void) { mutexLock(&registry); }
static void unlockStreams(void) { mutexUnlock(&registry); }
// The console of libnx, used when no sink is installed.
static int64_t consoleWrite(int fd, const void *bytes, size_t count, int *e) {
    FILE *stream = fd == 1 ? stdout : stderr;
    int saved = errno;
    errno = 0;
    size_t n = count ? fwrite(bytes, 1, count, stream) : 0;
    if (n < count || fflush(stream)) *e = LINUX_EIO;
    errno = saved;
    return *e && n == count ? -1 : (int64_t)n;
}
typedef struct {
    bool active, readable, writable, eof, error;
    int fd, direction;
    uint64_t generation;
    size_t begin, end, pending;
    unsigned char buffer[LINUX_STDIO_BUFFER];
} Stream;
static Stream streams[LINUX_STDIO_MAX];
static Stream standard[2] = {{.active = true, .writable = true, .fd = 1}, {.active = true, .writable = true, .fd = 2}};
void *linuxStdioStdout = &standard[0], *linuxStdioStderr = &standard[1];
static LinuxConsoleSink console_sink;
static void *console_context;
static _Atomic size_t active_count;
static uint64_t error_count;  // read by the line reader to tell a failure from the end of the data
typedef struct {
    bool read, write, append;
    int flags;
} Mode;
static int error(Stream *s, int value) {
    if (s) s->error = true;
    ++error_count;
    *linuxAbiErrnoLocation() = value;
    return -1;
}
static Stream *get(void *token) {
    for (unsigned i = 0; i < 2; ++i)
        if (token == &standard[i]) return &standard[i];
    for (unsigned i = 0; i < LINUX_STDIO_MAX; ++i)
        if (token == &streams[i] && streams[i].active) return &streams[i];
    return NULL;
}
static Stream *available(void) {
    for (unsigned i = 0; i < LINUX_STDIO_MAX; ++i)
        if (!streams[i].active) return &streams[i];
    return NULL;
}
static bool mode(const char *text, bool descriptor, Mode *m) {
    if (!text || !*text || (text[0] != 'r' && text[0] != 'w' && text[0] != 'a')) return false;
    unsigned seen = 0;
    for (const char *p = text + 1; *p; ++p) {
        unsigned bit = *p == '+' ? 1 : *p == 'b' ? 2 : *p == 'e' ? 4 : *p == 'x' ? 8 : 0;
        if (!bit || (seen & bit)) return false;
        seen |= bit;
    }
    if ((seen & 8) && (descriptor || text[0] != 'w')) return false;
    bool plus = (seen & 1) != 0;
    *m = (Mode){.read = text[0] == 'r' || plus, .write = text[0] != 'r' || plus, .append = text[0] == 'a'};
    m->flags = plus ? LINUX_O_RDWR : text[0] == 'r' ? LINUX_O_RDONLY : LINUX_O_WRONLY;
    if (text[0] == 'w') m->flags |= LINUX_O_CREAT | LINUX_O_TRUNC;
    if (text[0] == 'a') m->flags |= LINUX_O_CREAT | LINUX_O_APPEND;
    if (seen & 4) m->flags |= LINUX_O_CLOEXEC;
    if (seen & 8) m->flags |= LINUX_O_EXCL;
    return true;
}
static void initialize(Stream *s, int fd, uint64_t generation, const Mode *m) {
    *s = (Stream){.active = true, .fd = fd, .generation = generation, .readable = m->read, .writable = m->write};
    atomic_fetch_add(&active_count, 1);
}
static void release(Stream *s) {
    memset(s, 0, sizeof(*s));
    atomic_fetch_sub(&active_count, 1);
}
static int flushOutput(Stream *s) {
    while (s->pending) {
        int64_t n = linuxFileStreamWrite(s->fd, s->generation, s->buffer, s->pending);
        if (n < 0) return error(s, *linuxAbiErrnoLocation());
        if (!n || (uint64_t)n > s->pending) return error(s, LINUX_EIO);
        s->pending -= (size_t)n;
        memmove(s->buffer, s->buffer + n, s->pending);
    }
    return 0;
}
static int syncInput(Stream *s) {
    size_t unread = s->end - s->begin;
    if (unread && linuxFileStreamSeek(s->fd, s->generation, -(int64_t)unread, 1) < 0) return error(s, *linuxAbiErrnoLocation());
    s->begin = s->end = 0;
    s->direction = 0;
    return 0;
}
static int flush(Stream *s) { return s->direction == 1 ? syncInput(s) : flushOutput(s); }
static int closeStream(Stream *s) {
    if (s->fd < 3) return error(s, LINUX_EBADF);  // borrowed standard streams retained
    int first = flushOutput(s) ? *linuxAbiErrnoLocation() : 0;
    int result = linuxFileStreamClose(s->fd, s->generation);
    int closing = result ? *linuxAbiErrnoLocation() : 0;
    // A stale descriptor no longer belongs to this token. Never close its reuse.
    if (!closing || closing == LINUX_EBADF) release(s);
    if (first || closing) return error(s->active ? s : NULL, first ? first : closing);
    return 0;
}
void *linuxStdioFopen(const char *name, const char *text) {
    lockStreams();
    Mode m;
    Stream *s = NULL;
    void *result = NULL;
    if (!mode(text, false, &m))
        error(NULL, LINUX_EINVAL);
    else if (!(s = available()))
        error(NULL, LINUX_EMFILE);  // before truncation/creation
    else {
        int fd = linuxAbiOpen(name, m.flags, 0666);
        if (fd < 0)
            error(NULL, *linuxAbiErrnoLocation());
        else {
            uint64_t generation = 0;
            if (linuxFileStreamAttach(fd, m.read, m.write, m.append, &generation)) {
                int saved = *linuxAbiErrnoLocation();
                linuxAbiClose(fd);
                error(NULL, saved);
            } else {
                initialize(s, fd, generation, &m);
                result = s;
                if (m.append && !m.read && linuxFileStreamSeek(fd, generation, 0, 2) < 0) {
                    int saved = *linuxAbiErrnoLocation();
                    closeStream(s);
                    error(NULL, saved);
                    result = NULL;
                }
            }
        }
    }
    unlockStreams();
    return result;
}
void *linuxStdioFdopen(int fd, const char *text) {
    lockStreams();
    Mode m;
    Stream *s = NULL;
    void *result = NULL;
    uint64_t generation = 0;
    bool duplicate = false;
    for (unsigned i = 0; i < LINUX_STDIO_MAX; ++i)
        if (streams[i].active && streams[i].fd == fd) duplicate = true;
    if (!mode(text, true, &m))
        error(NULL, LINUX_EINVAL);
    else if (duplicate)
        error(NULL, LINUX_EBUSY);
    else if (!(s = available()))
        error(NULL, LINUX_EMFILE);
    else if (linuxFileStreamAttach(fd, m.read, m.write, m.append, &generation))
        error(NULL, *linuxAbiErrnoLocation());
    else {
        initialize(s, fd, generation, &m);
        result = s;
    }
    unlockStreams();
    return result;
}
int linuxStdioFclose(void *token) {
    lockStreams();
    Stream *s = get(token);
    int result = s ? closeStream(s) : error(NULL, LINUX_EBADF);
    unlockStreams();
    return result;
}
static size_t readBytes(Stream *s, unsigned char *out, size_t count) {
    if (!s->readable) {
        error(s, LINUX_EBADF);
        return 0;
    }
    if (s->direction == 2 && flushOutput(s)) return 0;
    s->direction = 1;
    size_t done = 0;
    while (done < count) {
        if (s->begin == s->end) {
            if (s->eof) break;
            int64_t n = linuxFileStreamRead(s->fd, s->generation, s->buffer, sizeof(s->buffer));
            if (n < 0) {
                error(s, *linuxAbiErrnoLocation());
                break;
            }
            if (!n) {
                s->eof = true;
                break;
            }
            s->begin = 0;
            s->end = (size_t)n;
        }
        size_t take = s->end - s->begin;
        if (take > count - done) take = count - done;
        memcpy(out + done, s->buffer + s->begin, take);
        s->begin += take;
        done += take;
    }
    return done;
}
static size_t writeBytes(Stream *s, const unsigned char *in, size_t count) {
    if (!s->writable) {
        error(s, LINUX_EBADF);
        return 0;
    }
    if (s->fd < 3) {
        int e = 0;
        int64_t n = console_sink ? console_sink(console_context, s->fd, in, count, &e) : consoleWrite(s->fd, in, count, &e);
        if (n < 0 || (uint64_t)n > count) {
            error(s, e ? e : LINUX_EIO);
            return 0;
        }

        if ((uint64_t)n < count) error(s, e ? e : LINUX_EIO);
        return (size_t)n;
    }
    if (s->direction == 1 && syncInput(s)) return 0;
    s->direction = 2;
    size_t done = 0;
    while (done < count) {
        if (s->pending == sizeof(s->buffer) && flushOutput(s)) break;
        size_t take = sizeof(s->buffer) - s->pending;
        if (take > count - done) take = count - done;
        memcpy(s->buffer + s->pending, in + done, take);
        s->pending += take;
        done += take;
    }
    return done;
}
static size_t transfer(void *buffer, size_t size, size_t count, void *token, bool writing) {
    if (!size || !count) return 0;
    lockStreams();
    Stream *s = get(token);
    size_t result = 0;
    if (!s)
        error(NULL, LINUX_EBADF);
    else if (count > SIZE_MAX / size || size * count > INT64_MAX)
        error(s, LINUX_EOVERFLOW);
    else if (!buffer)
        error(s, LINUX_EFAULT);
    else { result = (writing ? writeBytes(s, buffer, size * count) : readBytes(s, buffer, size * count)) / size; }
    unlockStreams();
    return result;
}
size_t linuxStdioFread(void *out, size_t size, size_t count, void *token) { return transfer(out, size, count, token, false); }
size_t linuxStdioFwrite(const void *in, size_t size, size_t count, void *token) { return transfer((void *)in, size, count, token, true); }
int64_t linuxStdioWriteRecord(void *token, const void *in, size_t bytes) {
    lockStreams();
    Stream *s = get(token);
    int64_t result = -1;
    if (!s)
        error(NULL, LINUX_EBADF);
    else if (!s->writable)
        error(s, LINUX_EBADF);
    else if (!in && bytes)
        error(s, LINUX_EFAULT);
    else if (bytes > INT64_MAX)
        error(s, LINUX_EOVERFLOW);
    else { result = (int64_t)writeBytes(s, in, bytes); }
    unlockStreams();
    return result;
}
// write(1|2, ...) issued directly by a client (not through a FILE): same destination as buffered console output.
int64_t linuxStdioWriteConsoleFd(int fd, const void *in, size_t count) {
    if (fd < 1 || fd > 2) {
        *linuxAbiErrnoLocation() = LINUX_EBADF;
        return -1;
    }
    int e = 0;
    LinuxConsoleSink sink = console_sink;
    void *context = console_context;
    int64_t n = sink ? sink(context, fd, in, count, &e) : consoleWrite(fd, in, count, &e);
    if (n < 0 || (uint64_t)n > count) {
        *linuxAbiErrnoLocation() = e ? e : LINUX_EIO;
        return -1;
    }
    return n;
}
void linuxStdioSetConsoleSink(LinuxConsoleSink sink, void *context) {
    lockStreams();
    console_sink = sink;
    console_context = context;
    unlockStreams();
}
int linuxStdioFflush(void *token) {
    lockStreams();
    int result = 0;
    if (token) {
        Stream *s = get(token);
        result = s ? flush(s) : error(NULL, LINUX_EBADF);
    } else {
        int first = 0;
        for (unsigned i = 0; i < LINUX_STDIO_MAX; ++i)
            if (streams[i].active && streams[i].direction == 2 && flushOutput(&streams[i]) && !first) first = *linuxAbiErrnoLocation();
        if (first) {
            *linuxAbiErrnoLocation() = first;
            result = -1;
        }
    }
    unlockStreams();
    return result;
}
static int flag(void *token, bool eof) {
    lockStreams();
    Stream *s = get(token);
    int result = 0;
    if (!s)
        error(NULL, LINUX_EBADF);
    else
        result = eof ? s->eof : s->error;
    unlockStreams();
    return result;
}
int linuxStdioFeof(void *token) { return flag(token, true); }
int linuxStdioFerror(void *token) { return flag(token, false); }
void linuxStdioClearerr(void *token) {
    lockStreams();
    Stream *s = get(token);
    if (!s)
        error(NULL, LINUX_EBADF);
    else
        s->eof = s->error = false;
    unlockStreams();
}
char *linuxStdioFgets(char *out, int bytes, void *token) {
    lockStreams();
    Stream *s = get(token);
    char *result = NULL;
    if (!s)
        error(NULL, LINUX_EBADF);
    else if (!out)
        error(s, LINUX_EFAULT);
    else if (bytes <= 0)
        error(s, LINUX_EINVAL);
    else if (bytes == 1) {
        out[0] = 0;
        result = out;
    } else {
        size_t length = 0;
        bool failed = false;
        while (length < (size_t)bytes - 1) {
            unsigned char c;
            uint64_t before = error_count;
            if (!readBytes(s, &c, 1)) {
                failed = error_count != before;
                break;
            }
            out[length++] = (char)c;
            if (c == '\n') break;
        }
        if (!failed && length) {
            out[length] = 0;
            result = out;
        }
    }
    unlockStreams();
    return result;
}
int linuxStdioFputs(const char *text, void *token) {
    lockStreams();
    Stream *s = get(token);
    int result = -1;
    if (!s)
        error(NULL, LINUX_EBADF);
    else if (!text)
        error(s, LINUX_EFAULT);
    else {
        size_t length = strlen(text);
        result = writeBytes(s, (const unsigned char *)text, length) == length ? 0 : -1;
    }
    unlockStreams();
    return result;
}
int linuxStdioFileno(void *token) {
    lockStreams();
    Stream *s = get(token);
    int result = s ? s->fd : error(NULL, LINUX_EBADF);
    unlockStreams();
    return result;
}
int linuxStdioFseeko(void *token, int64_t offset, int whence) {
    lockStreams();
    Stream *s = get(token);
    int result = -1;
    if (!s)
        error(NULL, LINUX_EBADF);
    else if (s->fd < 3)
        error(s, LINUX_ESPIPE);
    else if (whence < 0 || whence > 2)
        error(s, LINUX_EINVAL);
    else {

        if (!flushOutput(s)) {
            __int128 destination = (__int128)offset - (whence == 1 ? (s->end - s->begin) : 0);
            if (destination < INT64_MIN || destination > INT64_MAX)
                error(s, LINUX_EOVERFLOW);
            else if (linuxFileStreamSeek(s->fd, s->generation, (int64_t)destination, whence) < 0)
                error(s, *linuxAbiErrnoLocation());
            else {
                s->begin = s->end = 0;
                s->direction = 0;
                s->eof = false;
                result = 0;
            }
        }
    }
    unlockStreams();
    return result;
}
int64_t linuxStdioFtello(void *token) {
    lockStreams();
    Stream *s = get(token);
    int64_t result = -1;
    if (!s)
        error(NULL, LINUX_EBADF);
    else if (s->fd < 3)
        error(s, LINUX_ESPIPE);
    else {
        int64_t position = linuxFileStreamSeek(s->fd, s->generation, 0, 1);
        if (position < 0)
            error(s, *linuxAbiErrnoLocation());
        else {
            __int128 logical = (__int128)position + s->pending - (s->end - s->begin);
            if (logical < 0 || logical > INT64_MAX)
                error(s, LINUX_EOVERFLOW);
            else
                result = (int64_t)logical;
        }
    }
    unlockStreams();
    return result;
}
bool linuxStdioHasStreams(void) { return atomic_load(&active_count) != 0; }
bool linuxStdioReset(void) {
    lockStreams();
    bool ok = true;
    for (unsigned i = 0; i < LINUX_STDIO_MAX; ++i)
        if (streams[i].active && closeStream(&streams[i])) ok = false;
    if (ok) {
        error_count = 0;
        for (unsigned i = 0; i < 2; ++i) standard[i].eof = standard[i].error = false;
    }
    unlockStreams();
    return ok;
}
