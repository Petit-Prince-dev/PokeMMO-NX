#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *name, *version, *library;
    unsigned type, binding;
} ElfImport;
typedef bool (*ElfImportResolver)(void *context, const ElfImport *symbol, uintptr_t *address);
