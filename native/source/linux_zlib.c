#include "linux_zlib.h"
#include "linux_abi.h"
#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#include <zlib.h>

_Static_assert(sizeof(LinuxZStream) == 112 && offsetof(LinuxZStream, total_in) == 16 && offsetof(LinuxZStream, state) == 56 &&
                   offsetof(LinuxZStream, adler) == 96,
               "Linux LP64 z_stream");
_Static_assert(sizeof(LinuxGzHeader) == 80 && offsetof(LinuxGzHeader, time) == 8 && offsetof(LinuxGzHeader, comment) == 56, "Linux LP64 gz_header");
_Static_assert(sizeof(uLong) == 8 && sizeof(z_stream) == 112 && sizeof(gz_header) == 80, "Native zlib LP64 ABI");
_Static_assert(offsetof(z_stream, adler) == 96 && offsetof(gz_header, comment) == 56, "Native zlib field offsets");
typedef struct {
    bool active, busy, deflater;
    uintptr_t token;
    LinuxZStream *owner;
    z_stream native;
    LinuxZalloc allocate;
    LinuxZfree deallocate;
    void *opaque;
    LinuxGzHeader *header;
    gz_header native_header;
    uint64_t total_in, total_out;
} Record;
static Record records[LINUX_ZLIB_MAX];
static atomic_flag metadata = ATOMIC_FLAG_INIT;
static uintptr_t serial;
static void lock(void) {
    while (atomic_flag_test_and_set_explicit(&metadata, memory_order_acquire)) {}
}
static void unlock(void) { atomic_flag_clear_explicit(&metadata, memory_order_release); }
static void *defaultAllocate(void *opaque, unsigned count, unsigned bytes) {
    (void)opaque;
    return linuxAbiCalloc(count, bytes);
}
static void defaultFree(void *opaque, void *memory) {
    (void)opaque;
    linuxAbiFree(memory);
}
static void *allocate(void *opaque, unsigned count, unsigned bytes) {
    Record *r = opaque;
    void *p = r->allocate(r->opaque, count, bytes);
    return p;
}
static void deallocate(void *opaque, void *memory) {
    Record *r = opaque;
    if (!memory) return;
    r->deallocate(r->opaque, memory);
}
static Record *acquire(LinuxZStream *s, bool deflater) {
    if (!s) return NULL;
    lock();
    Record *result = NULL;
    for (unsigned i = 0; i < LINUX_ZLIB_MAX; ++i) {
        Record *r = &records[i];
        if (r->active && !r->busy && r->owner == s && r->token == (uintptr_t)s->state && r->deflater == deflater) {
            r->busy = true;
            result = r;
            break;
        }
    }
    unlock();
    return result;
}
static void headerInput(Record *r) {
    LinuxGzHeader *h = r->header;
    if (!h) return;
    r->native_header = (gz_header){.text = h->text,
                                   .time = (uLong)h->time,
                                   .xflags = h->xflags,
                                   .os = h->os,
                                   .extra = h->extra,
                                   .extra_len = h->extra_len,
                                   .extra_max = h->extra_max,
                                   .name = h->name,
                                   .name_max = h->name_max,
                                   .comment = h->comment,
                                   .comm_max = h->comm_max,
                                   .hcrc = h->hcrc,
                                   .done = h->done};
}
static void input(Record *r, const LinuxZStream *s) {
    r->native.next_in = (Bytef *)s->next_in;
    r->native.avail_in = s->avail_in;
    r->native.next_out = s->next_out;
    r->native.avail_out = s->avail_out;
}
static void output(Record *r, LinuxZStream *s, uLong before_in, uLong before_out) {
    z_stream *n = &r->native;
    r->total_in += (uLong)(n->total_in - before_in);
    r->total_out += (uLong)(n->total_out - before_out);
    s->next_in = n->next_in;
    s->avail_in = n->avail_in;
    s->total_in = r->total_in;
    s->next_out = n->next_out;
    s->avail_out = n->avail_out;
    s->total_out = r->total_out;
    s->msg = n->msg;
    s->state = n->state ? (void *)r->token : NULL;
    s->data_type = n->data_type;
    s->adler = n->adler;
    s->reserved = n->reserved;
}
static void release(Record *r, bool ended) {
    lock();
    if (ended) r->active = false;
    r->busy = false;
    unlock();
}
static int initialize(LinuxZStream *s, bool deflater, int level, int method, int window, int memory, int strategy, const char *version, int size) {
    if (!version || version[0] != ZLIB_VERSION[0] || size != 112) return Z_VERSION_ERROR;
    if (!s) return Z_STREAM_ERROR;
    lock();
    Record *r = NULL;
    bool duplicate = false;
    for (unsigned i = 0; i < LINUX_ZLIB_MAX; ++i) {
        if (records[i].active && records[i].owner == s) duplicate = true;
        if (!records[i].active && !records[i].busy && !r) r = &records[i];
    }
    if (duplicate || !r || serial == UINTPTR_MAX) {
        unlock();
        return duplicate ? Z_STREAM_ERROR : Z_MEM_ERROR;
    }
    *r = (Record){.active = true, .busy = true, .deflater = deflater, .owner = s, .token = ++serial};
    unlock();
    if (!s->zalloc) s->zalloc = defaultAllocate;
    if (!s->zfree) s->zfree = defaultFree;
    r->allocate = s->zalloc;
    r->deallocate = s->zfree;
    r->opaque = s->opaque;
    r->native.zalloc = allocate;
    r->native.zfree = deallocate;
    r->native.opaque = r;
    input(r, s);
    int result = deflater ? deflateInit2_(&r->native, level, method, window, memory, strategy, version, sizeof(z_stream))
                          : inflateInit2_(&r->native, window, version, sizeof(z_stream));
    output(r, s, 0, 0);
    release(r, !r->native.state);
    return result;
}
int linuxZlibDeflateInit2(LinuxZStream *s, int level, int method, int window, int memory, int strategy, const char *version, int size) {
    return initialize(s, true, level, method, window, memory, strategy, version, size);
}
int linuxZlibInflateInit2(LinuxZStream *s, int window, const char *version, int size) { return initialize(s, false, 0, 0, window, 0, 0, version, size); }
static int operate(LinuxZStream *s, bool deflater, int operation, int a, int b, const unsigned char *dictionary, unsigned length, LinuxGzHeader *header) {
    Record *r = acquire(s, deflater);
    if (!r) return Z_STREAM_ERROR;
    input(r, s);
    if (deflater && (operation == 0 || operation == 3)) headerInput(r);
    uLong in = r->native.total_in, out = r->native.total_out;
    int result;
    switch (operation) {
        case 0: result = deflater ? deflate(&r->native, a) : inflate(&r->native, a); break;
        case 1: result = deflater ? deflateEnd(&r->native) : inflateEnd(&r->native); break;
        case 2:
            result = deflater ? deflateReset(&r->native) : inflateReset(&r->native);
            if (result == Z_OK) {
                r->total_in = r->total_out = 0;
                in = out = 0;
            }
            break;
        case 3: result = deflateParams(&r->native, a, b); break;
        case 4: result = deflater ? deflateSetDictionary(&r->native, dictionary, length) : inflateSetDictionary(&r->native, dictionary, length); break;
        default:
            if (!header)
                result = Z_STREAM_ERROR;
            else {
                LinuxGzHeader *previous = r->header;
                r->header = header;
                headerInput(r);
                result = deflateSetHeader(&r->native, &r->native_header);
                if (result != Z_OK) r->header = previous;
            }
            break;
    }
    output(r, s, in, out);
    release(r, !r->native.state);
    return result;
}
int linuxZlibDeflate(LinuxZStream *s, int flush) { return operate(s, true, 0, flush, 0, NULL, 0, NULL); }
int linuxZlibInflate(LinuxZStream *s, int flush) { return operate(s, false, 0, flush, 0, NULL, 0, NULL); }
int linuxZlibDeflateEnd(LinuxZStream *s) { return operate(s, true, 1, 0, 0, NULL, 0, NULL); }
int linuxZlibInflateEnd(LinuxZStream *s) { return operate(s, false, 1, 0, 0, NULL, 0, NULL); }
int linuxZlibDeflateReset(LinuxZStream *s) { return operate(s, true, 2, 0, 0, NULL, 0, NULL); }
int linuxZlibInflateReset(LinuxZStream *s) { return operate(s, false, 2, 0, 0, NULL, 0, NULL); }
int linuxZlibDeflateParams(LinuxZStream *s, int level, int strategy) { return operate(s, true, 3, level, strategy, NULL, 0, NULL); }
int linuxZlibDeflateSetDictionary(LinuxZStream *s, const unsigned char *d, unsigned n) { return operate(s, true, 4, 0, 0, d, n, NULL); }
int linuxZlibInflateSetDictionary(LinuxZStream *s, const unsigned char *d, unsigned n) { return operate(s, false, 4, 0, 0, d, n, NULL); }
int linuxZlibDeflateSetHeader(LinuxZStream *s, LinuxGzHeader *h) { return operate(s, true, 5, 0, 0, NULL, 0, h); }
uint64_t linuxZlibDeflateBound(LinuxZStream *s, uint64_t length) {
    if (!s && length <= ULONG_MAX) return deflateBound(NULL, (uLong)length);
    Record *r = acquire(s, true);
    if (!r || length > ULONG_MAX) {
        if (r) release(r, false);
        return UINT64_MAX;
    }
    headerInput(r);
    uint64_t result = deflateBound(&r->native, (uLong)length);
    release(r, false);
    return result;
}
