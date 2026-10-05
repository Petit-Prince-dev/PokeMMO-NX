#include "elf_executable.h"
#include "diagnostics.h"
#include "elf_image.h"
#include <elf.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static void *allocateBacking(size_t size) { return aligned_alloc(ELF_PAGE_SIZE, size); }

static void releaseBacking(void *memory) { free(memory); }

bool elfExecutableClose(ElfExecutable *mapped) {
    bool ok = true;
    if (mapped->alias_backing) {
        Result rc =
            svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), (u64)(uintptr_t)mapped->base, (u64)(uintptr_t)mapped->alias_backing, mapped->alias_bytes);
        diagnosticsTrace("elf.mapping.alias_unmap rc=0x%x", rc);
        if (R_SUCCEEDED(rc)) {
            releaseBacking(mapped->alias_backing);
            mapped->alias_backing = NULL;
            mapped->alias_bytes = 0;
        } else
            ok = false;
    }
    for (unsigned i = mapped->layout.count; i-- > 0;) {
        ElfCodeRange *range = &mapped->ranges[i];
        Result rc;
        if (range->slave_mapped) {
            rc = svcControlCodeMemory(range->handle, CodeMapOperation_UnmapSlave, range->runtime, range->size, 0);
            diagnosticsTrace("elf.mapping.unmap_slave=%u rc=0x%x", i, rc);
            if (R_SUCCEEDED(rc))
                range->slave_mapped = false;
            else
                ok = false;
        }
        if (range->owner_mapped) {
            rc = svcControlCodeMemory(range->handle, CodeMapOperation_UnmapOwner, range->owner, range->size, 0);
            diagnosticsTrace("elf.mapping.unmap_owner=%u rc=0x%x", i, rc);
            if (R_SUCCEEDED(rc))
                range->owner_mapped = false;
            else
                ok = false;
        }
        if (range->slave_mapped || range->owner_mapped) continue;
        if (range->handle != INVALID_HANDLE) {
            rc = svcCloseHandle(range->handle);
            diagnosticsTrace("elf.mapping.close_handle=%u rc=0x%x", i, rc);
            if (R_FAILED(rc)) {
                ok = false;
                continue;
            }
            range->handle = INVALID_HANDLE;
        }
        releaseBacking(range->backing);
        range->backing = NULL;
    }
    if (ok) {
        if (mapped->reservation) {
            virtmemLock();
            virtmemRemoveReservation(mapped->reservation);
            virtmemUnlock();
        }
        memset(mapped, 0, sizeof(*mapped));
    }
    diagnosticsTrace("elf.mapping.close=%s", ok ? "PASS" : "FAIL retained=YES");
    return ok;
}

static bool openImage(const char *path, ElfExecutable *mapped, ElfImportResolver resolver, void *context, const ElfStageOptions *options);
bool elfExecutableOpenResolved(const char *path, ElfExecutable *mapped, ElfImportResolver resolver, void *context) {
    return openImage(path, mapped, resolver, context, NULL);
}
bool elfExecutableOpenLibrary(const char *path, ElfExecutable *mapped, ElfImportResolver resolver, void *context, uint64_t tls_offset,
                              uintptr_t tlsdesc_resolver) {
    ElfStageOptions options = {.library = true, .tls_offset = tls_offset, .tlsdesc_resolver = tlsdesc_resolver};
    return openImage(path, mapped, resolver, context, &options);
}
// hbloader-style mapping of a whole image: one backing block, one code-memory alias, then the permission of each segment.
static bool mapAsAlias(ElfExecutable *mapped, const ElfImage *staged) {
    size_t span = (mapped->layout.span + ELF_PAGE_SIZE - 1) & ~(size_t)(ELF_PAGE_SIZE - 1);
    void *backing = allocateBacking(span);
    if (!backing) return false;
    memset(backing, 0, span);
    for (unsigned i = 0; i < mapped->layout.count; ++i) {
        const ElfLoadSegment *segment = &mapped->layout.segments[i];
        size_t offset = segment->vaddr - mapped->layout.minimum_vaddr;
        if (offset > span || segment->memory_bytes > span - offset) {
            releaseBacking(backing);
            return false;
        }
        memcpy((unsigned char *)backing + offset, staged->memory + segment->vaddr - staged->minimum_vaddr, segment->memory_bytes);
    }
    armDCacheFlush(backing, span);
    Handle process = envGetOwnProcessHandle();
    Result rc = svcMapProcessCodeMemory(process, (u64)(uintptr_t)mapped->base, (u64)(uintptr_t)backing, span);
    diagnosticsTrace("elf.mapping.alias=%s base=%p backing=%p bytes=%zu rc=0x%x", R_SUCCEEDED(rc) ? "PASS" : "FAIL", mapped->base, backing, span, rc);
    if (R_FAILED(rc)) {
        releaseBacking(backing);
        return false;
    }
    mapped->alias_backing = backing;
    mapped->alias_bytes = span;
    for (unsigned i = 0; i < mapped->layout.count; ++i) {
        const ElfLoadSegment *segment = &mapped->layout.segments[i];
        ElfCodeRange *range = &mapped->ranges[i];
        range->size = segment->page_bytes;
        range->runtime = (unsigned char *)mapped->base + segment->page_vaddr - mapped->layout.minimum_vaddr;
        u32 permission = (segment->flags & PF_W) ? Perm_Rw : ((segment->flags & PF_X) ? Perm_Rx : Perm_R);
        rc = svcSetProcessMemoryPermission(process, (u64)(uintptr_t)range->runtime, range->size, permission);
        diagnosticsTrace("elf.mapping.alias_permission=%u address=%p bytes=%zu permission=%u rc=0x%x", i, range->runtime, range->size, permission, rc);
        if (R_FAILED(rc)) return false;
        if (segment->flags & PF_X) armICacheInvalidate(range->runtime, range->size);
        MemoryInfo info;
        u32 page_info;
        rc = svcQueryMemory(&info, &page_info, (u64)(uintptr_t)range->runtime);
        bool verified = R_SUCCEEDED(rc) && info.addr <= (uintptr_t)range->runtime && (uintptr_t)range->runtime - info.addr <= info.size &&
                        range->size <= info.size - ((uintptr_t)range->runtime - info.addr) && info.perm == permission;
        diagnosticsTrace("elf.mapping.permissions=%u result=%s rc=0x%x expected=0x%x actual=0x%x", i, verified ? "PASS" : "FAIL", rc, permission,
                         R_SUCCEEDED(rc) ? info.perm : 0);
        if (!verified) return false;
        if (memcmp((unsigned char *)range->runtime + segment->vaddr - segment->page_vaddr, staged->memory + segment->vaddr - staged->minimum_vaddr,
                   segment->memory_bytes)) {
            diagnosticsTrace("elf.mapping.contents=%u result=FAIL", i);
            return false;
        }
        diagnosticsTrace("elf.mapping.contents=%u result=PASS", i);
    }
    return true;
}

static bool openImage(const char *path, ElfExecutable *mapped, ElfImportResolver resolver, void *context, const ElfStageOptions *options) {
    bool library = options && options->library;
    memset(mapped, 0, sizeof(*mapped));
    for (unsigned i = 0; i < ELF_MAX_LOAD_SEGMENTS; ++i) mapped->ranges[i].handle = INVALID_HANDLE;
    ElfImage staged = {0};
    bool ok = false, main_executable = false;
    void *candidate = NULL;
    uintptr_t target = 0;
    if (library ? (!envIsSyscallHinted(0x73) || !envIsSyscallHinted(0x77) || !envIsSyscallHinted(0x78))
                : (!envIsSyscallHinted(0x4b) || !envIsSyscallHinted(0x4c))) {
        diagnosticsTrace("elf.mapping=UNAVAILABLE required system calls are not hinted");
        return false;
    }
    if (!elfReadLayout(path, &mapped->layout, library)) return false;
    virtmemLock();
    candidate = virtmemFindCodeMemory(mapped->layout.span + mapped->layout.alignment, ELF_PAGE_SIZE);
    if (candidate) {
        uintptr_t aligned = ((uintptr_t)candidate + mapped->layout.alignment - 1) & ~(uintptr_t)(mapped->layout.alignment - 1);
        mapped->base = (void *)aligned;
        mapped->reservation = virtmemAddReservation(mapped->base, mapped->layout.span);
    }
    virtmemUnlock();
    if (!mapped->reservation) goto done;
    target = (uintptr_t)mapped->base + mapped->layout.first_vaddr - mapped->layout.minimum_vaddr;
    if (!elfStageImage(path, &staged, target, resolver, context, options) || staged.unsupported) goto done;
    if (staged.minimum_vaddr != mapped->layout.first_vaddr) goto done;
    for (unsigned i = 0; i < mapped->layout.count; ++i) {
        ElfLoadSegment *segment = &mapped->layout.segments[i];
        if (segment->vaddr < staged.minimum_vaddr || segment->vaddr - staged.minimum_vaddr > staged.bytes ||
            segment->memory_bytes > staged.bytes - (segment->vaddr - staged.minimum_vaddr))
            goto done;
        if ((segment->flags & PF_X) && staged.main_vaddr >= segment->vaddr && staged.main_vaddr - segment->vaddr < segment->file_bytes) main_executable = true;
    }
    if (!main_executable && !library) goto done;
    mapped->relocated = staged.relocated;
    mapped->unresolved = staged.unresolved;
    mapped->unsupported = staged.unsupported;
    mapped->resolved = staged.resolved;
    mapped->weak_null = staged.weak_null;
    mapped->main_address = staged.main_vaddr ? (uintptr_t)mapped->base + staged.main_vaddr - mapped->layout.minimum_vaddr : 0;
    if (library) {
        uintptr_t bias = (uintptr_t)mapped->base - mapped->layout.minimum_vaddr;
        mapped->symtab = staged.symtab_vaddr ? bias + staged.symtab_vaddr : 0;
        mapped->strtab = staged.strtab_vaddr ? bias + staged.strtab_vaddr : 0;
        mapped->symbol_count = staged.symbol_count;
        mapped->strtab_bytes = staged.strtab_bytes;
        mapped->init = staged.init_vaddr ? bias + staged.init_vaddr : 0;
        mapped->init_array = staged.init_array_vaddr ? bias + staged.init_array_vaddr : 0;
        mapped->init_array_count = (unsigned)(staged.init_array_bytes / sizeof(uint64_t));
        memcpy(mapped->needed, staged.needed, sizeof(mapped->needed));
        mapped->needed_count = staged.needed_count;
    }
    mapped->export_count = staged.export_count;
    mapped->export_overflow = staged.export_overflow;
    for (unsigned i = 0; i < staged.export_count; ++i) {
        memcpy(mapped->exports[i].name, staged.exports[i].name, ELF_EXPORT_NAME_MAX);
        mapped->exports[i].address = (uintptr_t)mapped->base + staged.exports[i].vaddr - mapped->layout.minimum_vaddr;
        mapped->exports[i].type = staged.exports[i].type;
    }
    if (library) {
        if (!mapAsAlias(mapped, &staged)) goto done;
        mapped->verified = ok = true;
        goto done;
    }
    for (unsigned i = 0; i < mapped->layout.count; ++i) {
        ElfLoadSegment *segment = &mapped->layout.segments[i];
        ElfCodeRange *range = &mapped->ranges[i];
        range->size = segment->page_bytes;
        range->runtime = (unsigned char *)mapped->base + segment->page_vaddr - mapped->layout.minimum_vaddr;
        range->backing = allocateBacking(range->size);
        if (!range->backing) goto done;
        diagnosticsTrace("elf.mapping.stage=create segment=%u bytes=%zu", i, range->size);
        Result rc = svcCreateCodeMemory(&range->handle, range->backing, range->size);
        diagnosticsTrace("elf.mapping.create=%u rc=0x%x", i, rc);
        if (R_FAILED(rc)) goto done;
        virtmemLock();
        range->owner = (segment->flags & PF_W) ? range->runtime : virtmemFindCodeMemory(range->size, ELF_PAGE_SIZE);
        rc = range->owner ? svcControlCodeMemory(range->handle, CodeMapOperation_MapOwner, range->owner, range->size, Perm_Rw)
                          : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        virtmemUnlock();
        diagnosticsTrace("elf.mapping.owner=%u rc=0x%x", i, rc);
        if (R_FAILED(rc)) goto done;
        range->owner_mapped = true;
        // CreateCodeMemory clears/locks backing pages. Populate the mapped owner
        // AFTER creation, from an independent staged image with runtime pointers.
        memset(range->owner, 0, range->size);
        memcpy((unsigned char *)range->owner + segment->vaddr - segment->page_vaddr, staged.memory + segment->vaddr - staged.minimum_vaddr,
               segment->memory_bytes);
        armDCacheFlush(range->owner, range->size);
        if (!(segment->flags & PF_W)) {
            rc = svcControlCodeMemory(range->handle, CodeMapOperation_MapSlave, range->runtime, range->size, (segment->flags & PF_X) ? Perm_Rx : Perm_R);
            diagnosticsTrace("elf.mapping.slave=%u rc=0x%x", i, rc);
            if (R_FAILED(rc)) goto done;
            range->slave_mapped = true;
            if (segment->flags & PF_X) armICacheInvalidate(range->runtime, range->size);
            rc = svcControlCodeMemory(range->handle, CodeMapOperation_UnmapOwner, range->owner, range->size, 0);
            diagnosticsTrace("elf.mapping.seal=%u rc=0x%x", i, rc);
            if (R_FAILED(rc)) goto done;
            range->owner_mapped = false;
        }
        MemoryInfo info;
        u32 page_info;
        rc = svcQueryMemory(&info, &page_info, (u64)(uintptr_t)range->runtime);
        u32 expected = (segment->flags & PF_W) ? Perm_Rw : ((segment->flags & PF_X) ? Perm_Rx : Perm_R);
        bool verified = R_SUCCEEDED(rc) && info.addr <= (uintptr_t)range->runtime && (uintptr_t)range->runtime - info.addr <= info.size &&
                        range->size <= info.size - ((uintptr_t)range->runtime - info.addr) && info.perm == expected;
        diagnosticsTrace("elf.mapping.permissions=%u result=%s rc=0x%x expected=0x%x actual=0x%x", i, verified ? "PASS" : "FAIL", rc, expected,
                         R_SUCCEEDED(rc) ? info.perm : 0);
        if (!verified) goto done;
        if (memcmp((unsigned char *)range->runtime + segment->vaddr - segment->page_vaddr, staged.memory + segment->vaddr - staged.minimum_vaddr,
                   segment->memory_bytes)) {
            diagnosticsTrace("elf.mapping.contents=%u result=FAIL", i);
            goto done;
        }
        diagnosticsTrace("elf.mapping.contents=%u result=PASS", i);
    }
    mapped->verified = ok = true;
done:
    elfReleaseImage(&staged);
    diagnosticsTrace("elf.mapping=%s segments=%u relocated=%u unresolved=%u main=0x%llx entry_called=NO", ok ? "PASS" : "FAIL", mapped->layout.count,
                     mapped->relocated, mapped->unresolved, (unsigned long long)mapped->main_address);
    return ok;
}

uintptr_t elfExecutableFindExport(const ElfExecutable *mapped, const char *name) {
    for (unsigned i = 0; i < mapped->export_count; ++i)
        if (!strcmp(mapped->exports[i].name, name)) return mapped->exports[i].address;
    return 0;
}
