#pragma once
#include "elf_layout.h"
#include "elf_imports.h"
#include "elf_image.h"
#include <switch.h>

typedef struct {
    Handle handle;
    void *backing, *owner, *runtime;
    size_t size;
    bool owner_mapped, slave_mapped;
} ElfCodeRange;

typedef struct {
    char name[ELF_EXPORT_NAME_MAX];
    uintptr_t address;
    unsigned type;
} ElfExecutableExport;  // runtime address of an exported symbol

typedef struct {
    ElfLayout layout;
    ElfCodeRange ranges[ELF_MAX_LOAD_SEGMENTS];
    void *base;
    VirtmemReservation *reservation;
    uintptr_t main_address;
    // Shared libraries are mapped like hbloader maps an NRO (svcMapProcessCodeMemory + per-segment permissions):
    // no CodeMemory kernel object is used, because a process may only own about ten of them.
    void *alias_backing;
    size_t alias_bytes;
    uint32_t relocated, unresolved, unsupported, resolved, weak_null;
    bool verified;
    ElfExecutableExport exports[ELF_EXPORT_MAX];
    unsigned export_count, export_overflow;
    // Shared libraries: runtime addresses of the dynamic symbol table and initializers, and DT_NEEDED names.
    uintptr_t symtab, strtab, init, init_array;
    uint32_t symbol_count, strtab_bytes;
    unsigned init_array_count;
    char needed[ELF_NEEDED_MAX][ELF_NEEDED_NAME_MAX];
    unsigned needed_count;
} ElfExecutable;

// Runtime address of an exported symbol of the mapped image, or 0 when it has none.
uintptr_t elfExecutableFindExport(const ElfExecutable *mapped, const char *name);

// Mapping does not call entry points, constructors or unresolved imports.
// On partial failure, close the returned object; retry close if it fails.
bool elfExecutableOpenResolved(const char *path, ElfExecutable *mapped, ElfImportResolver resolver, void *context);
// A shared library: no main(), its TLS block already registered at tls_offset (when the layout has one) and
// tlsdesc_resolver the address of the TLS descriptor function. Nothing is executed by mapping.
bool elfExecutableOpenLibrary(const char *path, ElfExecutable *mapped, ElfImportResolver resolver, void *context, uint64_t tls_offset,
                              uintptr_t tlsdesc_resolver);
bool elfExecutableClose(ElfExecutable *mapped);
