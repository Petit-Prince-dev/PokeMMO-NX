#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define ELF_MAX_LOAD_SEGMENTS 16
#define ELF_PAGE_SIZE 4096

typedef struct {
    uint64_t vaddr, file_bytes, memory_bytes, page_vaddr, page_bytes;
    uint32_t flags;
} ElfLoadSegment;

typedef struct {
    ElfLoadSegment segments[ELF_MAX_LOAD_SEGMENTS];
    uint64_t minimum_vaddr, first_vaddr, span, alignment, entry;
    unsigned count;
    // PT_TLS (shared libraries): where the initialization image sits in the file and how big the block is.
    uint64_t program_header_offset;
    unsigned program_header_count;
    bool has_tls;
    uint64_t tls_file_offset, tls_file_bytes, tls_memory_bytes, tls_alignment;
} ElfLayout;

// The DT_NEEDED names of a shared object, read from the file (so the libraries it needs can be loaded before its own
// imports are bound). names are NUL-terminated entries of `name_bytes` each; false when the file cannot be read.
bool elfReadNeeded(const char *path, char *names, unsigned name_bytes, unsigned max, unsigned *count);
// Plan disjoint, page-aligned R/RX/RW mappings; reject overlapping or W+X pages.
// library=true accepts an image without an entry point (a shared object) and records its PT_TLS segment.
bool elfReadLayout(const char *path, ElfLayout *layout, bool library);
