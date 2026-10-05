#pragma once
#include <elf.h>
#include <stdio.h>
#include <stdbool.h>

typedef struct {
    const char *version, *library;
} ElfSymbolVersion;
// Returned names borrow dynstr storage. Caller frees the array.
bool elfReadVersions(FILE *file, uint64_t file_bytes, const Elf64_Shdr *sections, unsigned section_count, unsigned dynsym_index, const char *strings,
                     size_t string_bytes, size_t symbol_count, ElfSymbolVersion **out);
