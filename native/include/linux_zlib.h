#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void *(*LinuxZalloc)(void *, unsigned, unsigned);
typedef void (*LinuxZfree)(void *, void *);
typedef struct {
    const unsigned char *next_in;
    uint32_t avail_in, pad_in;
    uint64_t total_in;
    unsigned char *next_out;
    uint32_t avail_out, pad_out;
    uint64_t total_out;
    const char *msg;
    void *state;
    LinuxZalloc zalloc;
    LinuxZfree zfree;
    void *opaque;
    int32_t data_type;
    uint32_t pad_type;
    uint64_t adler, reserved;
} LinuxZStream;
typedef struct {
    int32_t text;
    uint32_t pad_time;
    uint64_t time;
    int32_t xflags, os;
    unsigned char *extra;
    uint32_t extra_len, extra_max;
    unsigned char *name;
    uint32_t name_max, pad_comment;
    unsigned char *comment;
    uint32_t comm_max;
    int32_t hcrc, done;
    uint32_t pad_end;
} LinuxGzHeader;
enum { LINUX_ZLIB_MAX = 32 };
int linuxZlibDeflateInit2(LinuxZStream *, int, int, int, int, int, const char *, int);
int linuxZlibInflateInit2(LinuxZStream *, int, const char *, int);
int linuxZlibDeflate(LinuxZStream *, int);
int linuxZlibInflate(LinuxZStream *, int);
int linuxZlibDeflateEnd(LinuxZStream *);
int linuxZlibInflateEnd(LinuxZStream *);
int linuxZlibDeflateReset(LinuxZStream *);
int linuxZlibInflateReset(LinuxZStream *);
int linuxZlibDeflateParams(LinuxZStream *, int, int);
uint64_t linuxZlibDeflateBound(LinuxZStream *, uint64_t);
int linuxZlibDeflateSetDictionary(LinuxZStream *, const unsigned char *, unsigned);
int linuxZlibInflateSetDictionary(LinuxZStream *, const unsigned char *, unsigned);
int linuxZlibDeflateSetHeader(LinuxZStream *, LinuxGzHeader *);
