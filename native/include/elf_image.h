#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "elf_imports.h"

// Defined global/weak function and object symbols of the dynamic symbol table. The official
// client exports about twenty, with names up to about eighty bytes.
#define ELF_EXPORT_MAX 48u
#define ELF_EXPORT_NAME_MAX 128u
typedef struct {
    char name[ELF_EXPORT_NAME_MAX];
    uint64_t vaddr;
    unsigned type;
} ElfExport;

#define ELF_NEEDED_MAX 16u
#define ELF_NEEDED_NAME_MAX 64u
// Input options for shared libraries. A library has no main(), brings its own TLS block (offset assigned by the
// caller before staging) and may use TLS descriptors, which need the resolver function's address.
typedef struct {
    bool library;
    uint64_t tls_offset;
    uintptr_t tlsdesc_resolver;
} ElfStageOptions;

typedef struct {
    unsigned char *memory;
    uint64_t bytes, minimum_vaddr, main_vaddr;
    uint32_t relocated, unresolved, unsupported, resolved, weak_null;
    ElfExport exports[ELF_EXPORT_MAX];
    unsigned export_count, export_overflow;  // overflow counts exports that did not fit or had a name too long
    // Libraries: the dynamic symbol table as mapped (virtual addresses), initializers and DT_NEEDED names.
    uint64_t symtab_vaddr, strtab_vaddr, init_vaddr, init_array_vaddr, init_array_bytes;
    uint32_t symbol_count, strtab_bytes;
    char needed[ELF_NEEDED_MAX][ELF_NEEDED_NAME_MAX];
    unsigned needed_count;
    uint32_t tls_descriptors;
} ElfImage;

// Stage PT_LOAD segments in ordinary memory, fix internal pointers and relocate the image to the address it will be mapped at (target_address:
// the runtime address of the lowest PT_LOAD byte, not a bias). Memory remains non-executable. Only imports accepted by the resolver are bound.
bool elfStageImage(const char *path, ElfImage *image, uintptr_t target_address, ElfImportResolver resolver, void *context, const ElfStageOptions *options);
void elfReleaseImage(ElfImage *image);
