#include "linux_dl.h"
#include "diagnostics.h"
#include "linux_abi.h"
#include "linux_files.h"
#include "linux_tls.h"
#include "linux_jit.h"
#include "linux_virtual_stubs.h"
#include <elf.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

extern void linuxTlsDescResolver(void);  // asm: ldr x0,[x0,#8]; ret

#define MAX_LIBRARIES 24u
typedef struct {
    bool used, is_virtual;
    char path[LINUX_FILE_PATH_MAX];
    ElfExecutable image;
    const LinuxVirtualLibrary *virtual_library;
    unsigned references;
} Library;
static Library libraries[MAX_LIBRARIES];
static RMutex lock;
static const LinuxVirtualLibrary *virtual_table;
static size_t virtual_count;
static char search_directory[LINUX_FILE_PATH_MAX] = "/lib";
static ElfImportResolver fallback;
static void *fallback_context;
static const ElfExecutable *main_image;
static char last_error[256];
static unsigned unresolved_total;

static void setError(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(last_error, sizeof(last_error), format, args);
    va_end(args);
    diagnosticsTrace("dl.error=%s", last_error);
}

void linuxDlSetVirtualLibraries(const LinuxVirtualLibrary *table, size_t count) {
    rmutexLock(&lock);
    virtual_table = table;
    virtual_count = table ? count : 0;
    rmutexUnlock(&lock);
}
void linuxDlSetSearchDirectory(const char *directory) {
    rmutexLock(&lock);
    snprintf(search_directory, sizeof(search_directory), "%s", directory ? directory : "/lib");
    rmutexUnlock(&lock);
}
void linuxDlSetFallbackResolver(ElfImportResolver resolver, void *context) {
    rmutexLock(&lock);
    fallback = resolver;
    fallback_context = context;
    rmutexUnlock(&lock);
}
void linuxDlSetMainImage(const ElfExecutable *image) {
    rmutexLock(&lock);
    main_image = image;
    rmutexUnlock(&lock);
}

static uintptr_t bias(const ElfExecutable *image) { return (uintptr_t)image->base - (uintptr_t)image->layout.minimum_vaddr; }
// Linear search of a dynamic symbol table that is already mapped (read-only): exported functions, objects
// and untyped symbols, global or weak. Thread-local variables are not addresses and are never returned.
static uintptr_t findSymbol(const void *symbols, uint32_t symbol_count, const char *strings, uint32_t string_bytes, uintptr_t load_bias, const char *name) {
    const Elf64_Sym *table = symbols;
    if (!table || !strings || !name || !*name) return 0;
    for (uint32_t i = 1; i < symbol_count; ++i) {
        const Elf64_Sym *symbol = &table[i];
        if (symbol->st_shndx == SHN_UNDEF || symbol->st_shndx == SHN_ABS || !symbol->st_value || symbol->st_name >= string_bytes) continue;
        unsigned binding = ELF64_ST_BIND(symbol->st_info), type = ELF64_ST_TYPE(symbol->st_info);
        if (binding != STB_GLOBAL && binding != STB_WEAK) continue;
        if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE) continue;
        size_t room = string_bytes - symbol->st_name;
        if (memchr(strings + symbol->st_name, 0, room) && strcmp(strings + symbol->st_name, name) == 0) return load_bias + symbol->st_value;
    }
    return 0;
}
static uintptr_t findInLibrary(const Library *library, const char *name) {
    uintptr_t address = findSymbol((const void *)library->image.symtab, library->image.symbol_count, (const char *)library->image.strtab,
                                   library->image.strtab_bytes, bias(&library->image), name);
    return linuxJitOverride(name, address);  // libffi's closure allocator is replaced (see linux_jit.h)
}
static const char *baseName(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Libraries the adapter registry itself stands for: never loaded from files.
static bool providedByRegistry(const char *name) {
    static const char *const provided[] = {"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1", "libz.so.1", "ld-linux-aarch64.so.1"};
    for (unsigned i = 0; i < sizeof(provided) / sizeof(provided[0]); ++i)
        if (!strcmp(name, provided[i])) return true;
    return false;
}

static bool resolveImport(void *context, const ElfImport *symbol, uintptr_t *address) {
    (void)context;
    *address = 0;
    if (symbol->type == STT_FUNC || symbol->type == STT_OBJECT || symbol->type == STT_NOTYPE) {
        // Libraries are linked against the same ABI as the client but were built against other glibc releases:
        // versions are not compared here (the client itself is still bound by exact version).
        uintptr_t found;
        if (linuxAbiLookup(symbol->name, &found)) {
            *address = found;
            return true;
        }
        for (unsigned i = 0; i < MAX_LIBRARIES; ++i)
            if (libraries[i].used && !libraries[i].is_virtual && (found = findInLibrary(&libraries[i], symbol->name))) {
                *address = found;
                return true;
            }
        for (unsigned i = 0; i < MAX_LIBRARIES; ++i)
            if (libraries[i].used && libraries[i].is_virtual && libraries[i].virtual_library->binds_imports &&
                (found = libraries[i].virtual_library->lookup(symbol->name))) {
                *address = found;
                return true;
            }
    }
    if (fallback && fallback(fallback_context, symbol, address) && *address) {
        ++unresolved_total;
        return true;
    }
    return false;
}

static Library *allocate(void) {
    for (unsigned i = 0; i < MAX_LIBRARIES; ++i)
        if (!libraries[i].used) {
            memset(&libraries[i], 0, sizeof(libraries[i]));
            libraries[i].used = true;
            return &libraries[i];
        }
    return NULL;
}
static void release(Library *library) { memset(library, 0, sizeof(*library)); }

static void *openLocked(const char *file, int flags, unsigned depth);

typedef void (*Constructor)(int, char **, char **);
static void runConstructors(const Library *library) {
    char **environment = linuxRuntimeEnviron;
    if (library->image.init) ((Constructor)library->image.init)(0, NULL, environment);
    const uint64_t *table = (const uint64_t *)library->image.init_array;
    for (unsigned i = 0; table && i < library->image.init_array_count; ++i)
        if (table[i] && table[i] != UINT64_MAX) ((Constructor)(uintptr_t)table[i])(0, NULL, environment);
}

// The client keeps a lock on the library it just extracted (its file is still open for writing) and Horizon refuses
// a second open of such a file. The file adapter shares the open handle, so read the library through it into a
// private copy that the loader can open directly.
static bool copyThroughAdapter(const char *virtual_path, char virtual_copy[LINUX_FILE_PATH_MAX], char native_copy[LINUX_FILE_PATH_MAX]) {
    static unsigned char buffer[65536];
    int source = linuxAbiOpen(virtual_path, LINUX_O_RDONLY, 0);
    if (source < 0) return false;
    snprintf(virtual_copy, LINUX_FILE_PATH_MAX, "/tmp/.library-copy");
    int target = linuxAbiOpen(virtual_copy, LINUX_O_WRONLY | LINUX_O_CREAT | LINUX_O_TRUNC, 0600);
    bool ok = target >= 0;
    while (ok) {
        int64_t count = linuxAbiRead(source, buffer, sizeof(buffer));
        if (count < 0) {
            ok = false;
            break;
        }
        if (count == 0) break;
        for (int64_t done = 0; done < count && ok;) {
            int64_t written = linuxAbiWrite(target, buffer + done, (size_t)(count - done));
            if (written <= 0)
                ok = false;
            else
                done += written;
        }
    }
    linuxAbiClose(source);
    if (target >= 0 && linuxAbiClose(target) != 0) ok = false;
    if (ok && linuxFilesNativePath(virtual_copy, native_copy) != 0) ok = false;
    if (!ok) {
        if (target >= 0) linuxAbiUnlink(virtual_copy);
        virtual_copy[0] = 0;
    }
    return ok;
}

// Maps the library whose bytes are in the native file `native`; `virtual_path` is the name the client knows it by.
static void *openFromNative(const char *file, const char *virtual_path, const char *native, unsigned depth) {
    (void)depth;
    ElfLayout layout;
    if (!elfReadLayout(native, &layout, true)) {
        setError("%s: not a loadable AArch64 shared object", file);
        return NULL;
    }
    uint64_t tls_offset = 0;
    if (layout.has_tls) {
        void *image = layout.tls_file_bytes ? malloc(layout.tls_file_bytes) : NULL;
        FILE *source = fopen(native, "rb");
        bool read = source && (!layout.tls_file_bytes || (image && fseek(source, (long)layout.tls_file_offset, SEEK_SET) == 0 &&
                                                          fread(image, 1, layout.tls_file_bytes, source) == layout.tls_file_bytes));
        if (source) fclose(source);
        bool registered = read && linuxTlsRegisterModule(image, layout.tls_file_bytes, layout.tls_memory_bytes, layout.tls_alignment, &tls_offset);
        free(image);
        if (!registered) {
            setError("%s: no room for its thread-local storage", file);
            return NULL;
        }
        diagnosticsTrace("dl.tls file=%s offset=%llu bytes=%llu", file, (unsigned long long)tls_offset, (unsigned long long)layout.tls_memory_bytes);
    }
    Library *library = allocate();
    if (!library) {
        setError("%s: too many libraries", file);
        return NULL;
    }
    snprintf(library->path, sizeof(library->path), "%s", virtual_path);
    library->references = 1;
    unsigned unresolved_before = unresolved_total;
    if (!elfExecutableOpenLibrary(native, &library->image, resolveImport, NULL, tls_offset, (uintptr_t)linuxTlsDescResolver)) {
        setError("%s: mapping failed", file);
        elfExecutableClose(&library->image);
        release(library);
        return NULL;
    }
    diagnosticsTrace("dl.mapped file=%s base=%p span=0x%llx symbols=%u needed=%u init=%s init_array=%u fallback_stubs=%u", file, library->image.base,
                     (unsigned long long)library->image.layout.span, library->image.symbol_count, library->image.needed_count,
                     library->image.init ? "yes" : "no", library->image.init_array_count, unresolved_total - unresolved_before);
    if (library->image.unresolved) { setError("%s: %u unresolved imports", file, library->image.unresolved); }
    runConstructors(library);
    diagnosticsTrace("dl.opened file=%s", file);
    return library;
}

static void *openLocked(const char *file, int flags, unsigned depth) {
    (void)flags;
    if (depth > 8) {
        setError("%s: dependency chain too deep", file);
        return NULL;
    }
    const char *name = baseName(file);
    for (size_t i = 0; i < virtual_count; ++i) {
        if (strncmp(name, virtual_table[i].prefix, strlen(virtual_table[i].prefix))) continue;
        for (unsigned j = 0; j < MAX_LIBRARIES; ++j)
            if (libraries[j].used && libraries[j].is_virtual && libraries[j].virtual_library == &virtual_table[i]) return &libraries[j];
        Library *library = allocate();
        if (!library) {
            setError("%s: too many libraries", file);
            return NULL;
        }
        library->is_virtual = true;
        library->virtual_library = &virtual_table[i];
        snprintf(library->path, sizeof(library->path), "%s", file);
        diagnosticsTrace("dl.virtual=%s file=%s", virtual_table[i].prefix, file);
        return library;
    }
    char virtual_path[LINUX_FILE_PATH_MAX], native[LINUX_FILE_PATH_MAX];
    if (strchr(file, '/'))
        snprintf(virtual_path, sizeof(virtual_path), "%s", file);
    else {
        size_t directory_length = strlen(search_directory), name_length = strlen(file);
        if (directory_length + 1 + name_length >= sizeof(virtual_path)) {
            setError("%s: name too long", file);
            return NULL;
        }
        memcpy(virtual_path, search_directory, directory_length);
        virtual_path[directory_length] = '/';
        memcpy(virtual_path + directory_length + 1, file, name_length + 1);
    }
    if (linuxFilesNativePath(virtual_path, native) != 0) {
        setError("%s: cannot open shared object file", file);
        return NULL;
    }
    for (unsigned i = 0; i < MAX_LIBRARIES; ++i)
        if (libraries[i].used && !libraries[i].is_virtual && !strcmp(libraries[i].path, virtual_path)) {
            ++libraries[i].references;
            return &libraries[i];
        }
    char virtual_copy[LINUX_FILE_PATH_MAX];
    virtual_copy[0] = 0;
    FILE *existing = fopen(native, "rb");
    if (existing)
        fclose(existing);
    else {
        int open_errno = errno;
        unsigned fs_result = fsdevGetLastResult();
        char copy_native[LINUX_FILE_PATH_MAX];
        if (!copyThroughAdapter(virtual_path, virtual_copy, copy_native)) {
            diagnosticsTrace("dl.open_failed file=%s native_errno=%d fs_rc=0x%x", file, open_errno, fs_result);
            setError("%s: cannot open shared object file: No such file or directory", file);
            return NULL;
        }
        diagnosticsTrace("dl.copied file=%s reason=native_open_failed native_errno=%d fs_rc=0x%x", file, open_errno, fs_result);
        snprintf(native, sizeof(native), "%s", copy_native);
    }
    // The libraries this one needs go first: its imports are bound while it is mapped, and a symbol of a library that is
    // not loaded yet would be bound to a refusing stub for good (the C++ allocator of libstdc++ then returned -1 and the game crashed).
    char needed[ELF_NEEDED_MAX][ELF_NEEDED_NAME_MAX];
    unsigned needed_count = 0;
    elfReadNeeded(native, &needed[0][0], ELF_NEEDED_NAME_MAX, ELF_NEEDED_MAX, &needed_count);
    for (unsigned i = 0; i < needed_count; ++i) {
        if (providedByRegistry(needed[i])) continue;
        bool virtual_match = false, binds = false;
        for (size_t j = 0; j < virtual_count; ++j)
            if (!strncmp(needed[i], virtual_table[j].prefix, strlen(virtual_table[j].prefix))) {
                virtual_match = true;
                binds = binds || virtual_table[j].binds_imports;
            }
        if (virtual_match) {
            if (binds) openLocked(needed[i], 0, depth + 1);  // a library that provides imports must be there before the one that needs it is mapped
            continue;
        }
        if (!openLocked(needed[i], 0, depth + 1))
            diagnosticsTrace("dl.needed_missing file=%s needed=%s", file, needed[i]);
        else
            diagnosticsTrace("dl.needed file=%s needed=%s", file, needed[i]);
    }
    void *result = openFromNative(file, virtual_path, native, depth);
    if (virtual_copy[0]) linuxAbiUnlink(virtual_copy);
    return result;
}

static void *linuxDlOpen(const char *file, int flags) {
    if (!file || !*file) return NULL;
    rmutexLock(&lock);
    last_error[0] = 0;
    void *result = openLocked(file, flags, 0);
    rmutexUnlock(&lock);
    return result;
}

static void *linuxDlSymbol(void *handle, const char *name) {
    if (!name) return NULL;
    rmutexLock(&lock);
    uintptr_t address = 0;
    if (!handle) {
        for (unsigned i = 0; i < MAX_LIBRARIES && !address; ++i) {
            if (!libraries[i].used) continue;
            address = libraries[i].is_virtual ? libraries[i].virtual_library->lookup(name) : findInLibrary(&libraries[i], name);
        }
    } else {
        Library *library = handle;
        if (library >= libraries && library < libraries + MAX_LIBRARIES && library->used) {
            address = library->is_virtual ? library->virtual_library->lookup(name) : findInLibrary(library, name);
            const char *prefix = library->is_virtual ? library->virtual_library->stub_prefix : NULL;
            if (!address && prefix && !strncmp(name, prefix, strlen(prefix))) address = linuxVirtualStub(name);
        } else
            setError("invalid library handle");
    }
    rmutexUnlock(&lock);
    return (void *)address;
}

static int linuxDlClose(void *handle) {
    (void)handle;
    return 0;
}
static const char *linuxDlError(void) {
    rmutexLock(&lock);
    static _Thread_local char copy[256];
    const char *result = NULL;
    if (last_error[0]) {
        snprintf(copy, sizeof(copy), "%s", last_error);
        last_error[0] = 0;
        result = copy;
    }
    rmutexUnlock(&lock);
    return result;
}

// struct dl_phdr_info of glibc (64 bytes on AArch64).
typedef struct {
    uint64_t address;
    const char *name;
    const void *program_headers;
    uint16_t program_header_count;
    uint16_t padding[3];
    uint64_t adds, subs, tls_module_id;
    void *tls_data;
} PhdrInfo;
_Static_assert(sizeof(PhdrInfo) == 64, "glibc dl_phdr_info");
static int linuxDlIteratePhdr(int (*callback)(void *, size_t, void *), void *data) {
    rmutexLock(&lock);
    int result = 0;
    PhdrInfo info;
    if (main_image && main_image->base) {
        memset(&info, 0, sizeof(info));
        info.address = bias(main_image);
        info.name = "";
        info.program_headers = (const void *)(bias(main_image) + main_image->layout.program_header_offset);
        info.program_header_count = (uint16_t)main_image->layout.program_header_count;
        result = callback(&info, sizeof(info), data);
    }
    for (unsigned i = 0; i < MAX_LIBRARIES && !result; ++i) {
        if (!libraries[i].used || libraries[i].is_virtual) continue;
        memset(&info, 0, sizeof(info));
        info.address = bias(&libraries[i].image);
        info.name = libraries[i].path;
        info.program_headers = (const void *)(bias(&libraries[i].image) + libraries[i].image.layout.program_header_offset);
        info.program_header_count = (uint16_t)libraries[i].image.layout.program_header_count;
        result = callback(&info, sizeof(info), data);
    }
    rmutexUnlock(&lock);
    return result;
}

unsigned linuxDlRegions(LinuxDlRegion *out, unsigned max) {
    unsigned count = 0;
    for (unsigned i = 0; i < MAX_LIBRARIES && count < max; ++i) {
        const Library *library = &libraries[i];
        if (!library->used || library->is_virtual || !library->image.base) continue;
        const char *name = strrchr(library->path, '/');
        out[count++] = (LinuxDlRegion){name ? name + 1 : library->path, (uintptr_t)library->image.base, library->image.layout.span};
    }
    return count;
}
LinuxDlStats linuxDlStats(void) {
    LinuxDlStats stats = {0};
    rmutexLock(&lock);
    for (unsigned i = 0; i < MAX_LIBRARIES; ++i)
        if (libraries[i].used) {
            if (libraries[i].is_virtual)
                ++stats.virtual_libraries;
            else
                ++stats.libraries;
        }
    stats.unresolved_imports = unresolved_total;
    stats.tls_modules = linuxTlsModuleCount();
    rmutexUnlock(&lock);
    return stats;
}

bool linuxDlReset(void) {
    rmutexLock(&lock);
    linuxVirtualStubsReset();
    linuxJitReset();
    bool ok = true;
    for (unsigned i = MAX_LIBRARIES; i-- > 0;) {
        if (!libraries[i].used) continue;
        if (!libraries[i].is_virtual && !elfExecutableClose(&libraries[i].image)) {
            ok = false;
            continue;
        }
        release(&libraries[i]);
    }
    if (ok) {
        unresolved_total = 0;
        last_error[0] = 0;
    }
    rmutexUnlock(&lock);
    return ok;
}

static const LinuxDynamicLoader hooks = {linuxDlOpen, linuxDlSymbol, linuxDlClose, linuxDlError, linuxDlIteratePhdr};
const LinuxDynamicLoader *linuxDlLoaderHooks(void) { return &hooks; }
