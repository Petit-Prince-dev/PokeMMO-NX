#pragma once
#include "elf_executable.h"
#include "elf_imports.h"
#include "linux_runtime.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Run-time loading of shared libraries for the guest (Linux AArch64, glibc ABI).
//
// A library named by a path of the guest's virtual file system is mapped like the client image (CodeMemory),
// its static TLS block is registered, its imports are bound (the adapter registry by name, then libraries
// loaded earlier, then an optional fallback resolver) and its constructors run on the calling thread.
// Libraries this project implements itself (SDL3, EGL, OpenAL, GTK) are "virtual": dlopen of a matching file name
// returns a handle whose dlsym is answered by that library's lookup function. Nothing here is thread-safe against
// linuxDlReset: reset only when no guest thread runs.
typedef struct {
    const char *prefix;                     // matched against the start of the file's base name, e.g. "libSDL3"
    uintptr_t (*lookup)(const char *name);  // address of the function or object, 0 when unknown
    const char *stub_prefix;                // optional: names with this prefix that lookup does not know get a logging stub (see linux_virtual_stubs.h)
    bool binds_imports;                     // the imports of the libraries loaded afterwards are bound to this library's functions too
} LinuxVirtualLibrary;

void linuxDlSetVirtualLibraries(const LinuxVirtualLibrary *table, size_t count);
void linuxDlSetSearchDirectory(const char *guest_directory);                 // where bare names (DT_NEEDED) are looked for
void linuxDlSetFallbackResolver(ElfImportResolver resolver, void *context);  // e.g. the refusing stubs of linux_trap.c
void linuxDlSetMainImage(const ElfExecutable *main_image);                   // for dl_iterate_phdr

typedef struct {
    const char *name;
    uintptr_t base;
    uint64_t span;
} LinuxDlRegion;
unsigned linuxDlRegions(LinuxDlRegion *out, unsigned max);  // loaded libraries, read without a lock (diagnostics)
typedef struct {
    unsigned libraries, virtual_libraries, unresolved_imports, tls_modules;
} LinuxDlStats;
LinuxDlStats linuxDlStats(void);
bool linuxDlReset(void);  // unmaps every library; false (and the rest kept) when an unmap fails

// The ready-made hook table for linuxRuntimeSetDynamicLoader.
const LinuxDynamicLoader *linuxDlLoaderHooks(void);
