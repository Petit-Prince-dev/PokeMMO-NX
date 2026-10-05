#include "linux_sdl_io.h"
#include <stdatomic.h>
#include <string.h>

#define STREAMS 8u
typedef struct {
    uint8_t *data;
    int64_t size, position;
    bool used, writable;
} Stream;
static Stream streams[STREAMS];
static atomic_flag lock = ATOMIC_FLAG_INIT;
static void lockStreams(void) {
    while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire)) {}
}
static void unlockStreams(void) { atomic_flag_clear_explicit(&lock, memory_order_release); }

static Stream *find(void *stream) {  // lock held
    for (unsigned i = 0; i < STREAMS; ++i)
        if (stream == &streams[i] && streams[i].used) return &streams[i];
    return NULL;
}
void *linuxSdlIoFromMemory(void *memory, size_t size, bool writable) {
    if (!memory || size > (size_t)INT64_MAX) return NULL;
    void *result = NULL;
    lockStreams();
    for (unsigned i = 0; i < STREAMS && !result; ++i)
        if (!streams[i].used) {
            streams[i] = (Stream){(uint8_t *)memory, (int64_t)size, 0, true, writable};
            result = &streams[i];
        }
    unlockStreams();
    return result;
}
size_t linuxSdlIoRead(void *stream, void *buffer, size_t size) {
    lockStreams();
    Stream *s = find(stream);
    size_t count = 0;
    if (s && buffer) {
        int64_t remaining = s->size - s->position;
        count = (int64_t)size < remaining ? size : (size_t)remaining;
        memcpy(buffer, s->data + s->position, count);
        s->position += (int64_t)count;
    }
    unlockStreams();
    return count;
}
size_t linuxSdlIoWrite(void *stream, const void *buffer, size_t size) {
    lockStreams();
    Stream *s = find(stream);
    size_t count = 0;
    if (s && s->writable && buffer) {
        int64_t remaining = s->size - s->position;
        count = (int64_t)size < remaining ? size : (size_t)remaining;
        memcpy(s->data + s->position, buffer, count);
        s->position += (int64_t)count;
    }
    unlockStreams();
    return count;
}
int64_t linuxSdlIoSeek(void *stream, int64_t offset, int whence) {
    lockStreams();
    Stream *s = find(stream);
    int64_t result = -1;
    if (s && whence >= 0 && whence <= 2) {
        int64_t base = whence == 0 ? 0 : (whence == 1 ? s->position : s->size);
        int64_t target = base + offset;
        if (target < 0) target = 0;
        if (target > s->size) target = s->size;
        s->position = result = target;
    }
    unlockStreams();
    return result;
}
int64_t linuxSdlIoTell(void *stream) {
    lockStreams();
    Stream *s = find(stream);
    int64_t value = s ? s->position : -1;
    unlockStreams();
    return value;
}
int64_t linuxSdlIoSize(void *stream) {
    lockStreams();
    Stream *s = find(stream);
    int64_t value = s ? s->size : -1;
    unlockStreams();
    return value;
}
bool linuxSdlIoClose(void *stream) {
    lockStreams();
    Stream *s = find(stream);
    if (s) memset(s, 0, sizeof(*s));
    unlockStreams();
    return s != NULL;
}
int linuxSdlIoCountMappings(void *stream) {
    lockStreams();
    Stream *s = find(stream);
    int count = -1;
    if (s) {
        count = 0;
        const uint8_t *p = s->data + s->position, *end = s->data + s->size;
        while (p < end) {
            const uint8_t *line = p;
            while (p < end && *p != '\n') ++p;
            const uint8_t *stop = p;
            while (line < stop && (*line == ' ' || *line == '\t' || *line == '\r')) ++line;
            bool has_comma = false;
            for (const uint8_t *c = line; c < stop && !has_comma; ++c) has_comma = *c == ',';
            if (line < stop && *line != '#' && has_comma) ++count;
            if (p < end) ++p;
        }
        s->position = s->size;
    }
    unlockStreams();
    return count;
}
void linuxSdlIoReset(void) {
    lockStreams();
    memset(streams, 0, sizeof(streams));
    unlockStreams();
}
