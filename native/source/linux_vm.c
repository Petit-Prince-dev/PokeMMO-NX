#include "linux_vm.h"
#include "linux_abi.h"
#include "linux_files.h"
#include "diagnostics.h"
#include <stdlib.h>
#include <string.h>
#include <switch.h>

// ---- the kernel's side: process code memory, address reservations, permissions -------------------------------------------------------
// Operations return Linux errno values. Map success locks the source pages and creates an inaccessible alias. Convert success changes that
// alias to ordinary re-protectable data (initially RW). Backing is freed ONLY after every corresponding alias has been unmapped.
static Mutex vm_lock;
static void lockVm(void) { mutexLock(&vm_lock); }
static void unlockVm(void) { mutexUnlock(&vm_lock); }
static bool backendAvailable(void) {
    return envGetOwnProcessHandle() != INVALID_HANDLE && envGetOwnProcessHandle() != CUR_PROCESS_HANDLE && envIsSyscallHinted(0x02) &&
           envIsSyscallHinted(0x73) && envIsSyscallHinted(0x77) && envIsSyscallHinted(0x78);
}
static void traceAddressSpace(void) {
    static const struct {
        const char *name;
        InfoType base, size;
    } regions[] = {{"aslr", InfoType_AslrRegionAddress, InfoType_AslrRegionSize},
                   {"alias", InfoType_AliasRegionAddress, InfoType_AliasRegionSize},
                   {"heap", InfoType_HeapRegionAddress, InfoType_HeapRegionSize},
                   {"stack", InfoType_StackRegionAddress, InfoType_StackRegionSize}};
    for (size_t i = 0; i < sizeof(regions) / sizeof(*regions); ++i) {
        u64 base = 0, size = 0;
        Result rc = svcGetInfo(&base, regions[i].base, CUR_PROCESS_HANDLE, 0);
        if (R_SUCCEEDED(rc)) rc = svcGetInfo(&size, regions[i].size, CUR_PROCESS_HANDLE, 0);
        diagnosticsTrace("vm.space region=%s base=0x%llx size=0x%llx rc=0x%x", regions[i].name, (unsigned long long)base, (unsigned long long)size, rc);
    }
}
// libnx draws random addresses until one avoids the alias and heap regions, and never returns when there is none. In a 36-bit address space
// (a forwarder title) those regions can sit anywhere, so that no big window is left. True when at least 1/1024 of the possible start addresses
// work: the draw then ends at once.
typedef struct {
    u64 low, high;
} Span;  // closed; empty when low > high
static u64 spanLength(Span span) { return span.low > span.high ? 0 : span.high - span.low + 1; }
static Span spanClip(Span span, Span window) {
    return (Span){span.low < window.low ? window.low : span.low, span.high > window.high ? window.high : span.high};
}
// The start addresses of a window of `length` bytes that would overlap `region` (first and one past the last byte).
static Span spanForbidden(const u64 region[2], u64 length, Span starts) {
    if (region[1] <= region[0]) return (Span){1, 0};
    return spanClip((Span){region[0] >= length ? region[0] - length + 1 : 0, region[1] - 1}, starts);
}
static bool startsAvailable(const u64 aslr[2], const u64 alias[2], const u64 heap[2], size_t bytes) {
    u64 length = ((u64)bytes + 0xFFF) & ~(u64)0xFFF;
    if (length > aslr[1] - aslr[0]) return false;
    Span starts = {aslr[0], aslr[1] - length}, a = spanForbidden(alias, length, starts), h = spanForbidden(heap, length, starts);
    Span both = {a.low > h.low ? a.low : h.low, a.high < h.high ? a.high : h.high};
    u64 total = spanLength(starts), forbidden = spanLength(a) + spanLength(h) - spanLength(both);
    return (total - forbidden) * 1024 >= total;
}
static bool regionBounds(InfoType base_type, InfoType size_type, u64 bounds[2]) {
    u64 size = 0;
    Result rc = svcGetInfo(&bounds[0], base_type, CUR_PROCESS_HANDLE, 0);
    if (R_SUCCEEDED(rc)) rc = svcGetInfo(&size, size_type, CUR_PROCESS_HANDLE, 0);
    bounds[1] = bounds[0] + size;
    return R_SUCCEEDED(rc);
}
static bool placementPossible(size_t bytes) {
    u64 aslr[2], alias[2], heap[2], extra = 0;
    if (!regionBounds(InfoType_AslrRegionAddress, InfoType_AslrRegionSize, aslr) ||
        !regionBounds(InfoType_AliasRegionAddress, InfoType_AliasRegionSize, alias) || !regionBounds(InfoType_HeapRegionAddress, InfoType_HeapRegionSize, heap))
        return true;  // an old kernel: leave it to libnx
    if (R_SUCCEEDED(svcGetInfo(&extra, InfoType_AliasRegionExtraSize, CUR_PROCESS_HANDLE, 0)) && extra <= alias[1] - alias[0]) alias[1] -= extra;
    return startsAvailable(aslr, alias, heap, bytes);
}
// Reserves `bytes` of address space; when that does not fit, a reservation of at least 2 * LINUX_VM_MIN_WINDOW is halved until it does (never
// below LINUX_VM_MIN_WINDOW). `granted` is what was reserved.
static int nativeReserve(size_t bytes, void **address, void **reservation, size_t *granted) {
    static bool traced;
    if (!traced && bytes >= LINUX_VM_MIN_WINDOW) {
        traced = true;
        traceAddressSpace();
    }
    size_t size = bytes;
    virtmemLock();
    for (;;) {
        *address = placementPossible(size) ? virtmemFindCodeMemory(size, LINUX_VM_PAGE) : NULL;
        if (*address || size / 2 < LINUX_VM_MIN_WINDOW) break;
        size = (size / 2) & ~(size_t)(LINUX_VM_PAGE - 1);
    }
    *reservation = *address ? virtmemAddReservation(*address, size) : NULL;
    virtmemUnlock();
    if (!*reservation)
        diagnosticsTrace("vm.reserve=FAIL address=%p bytes=%zu", *address, size);
    else if (size < bytes)
        diagnosticsTrace("vm.reserve=CLIPPED address=%p requested=%zu granted=%zu", *address, bytes, size);
    *granted = size;
    return *reservation ? 0 : LINUX_ENOMEM;
}
static void nativeRelease(void *reservation) {
    virtmemLock();
    virtmemRemoveReservation(reservation);
    virtmemUnlock();
}
static void *nativeAllocate(size_t bytes) { return aligned_alloc(LINUX_VM_PAGE, bytes); }
static void nativeFree(void *address) { free(address); }
static int nativeMap(void *destination, void *source, size_t bytes) {
    // Publish initialization before the kernel locks the source and installs
    // its alias. Never access source while the alias is mapped.
    armDCacheFlush(source, bytes);
    Result rc = svcMapProcessCodeMemory(envGetOwnProcessHandle(), (u64)(uintptr_t)destination, (u64)(uintptr_t)source, bytes);
    if (R_FAILED(rc)) diagnosticsTrace("vm.map=FAIL dst=%p src=%p bytes=%zu rc=0x%x", destination, source, bytes, rc);
    return R_SUCCEEDED(rc) ? 0 : LINUX_ENOMEM;
}
static int nativeConvert(void *address, size_t bytes) {
    // AliasCode -> AliasCodeData: only the latter has CanReprotect. Subsequent
    // R/RW/NONE transitions use svcSetMemoryPermission, NOT syscall 0x73.
    Result rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), (u64)(uintptr_t)address, bytes, Perm_Rw);
    if (R_FAILED(rc)) diagnosticsTrace("vm.convert_data=FAIL address=%p bytes=%zu rc=0x%x", address, bytes, rc);
    return R_SUCCEEDED(rc) ? 0 : LINUX_EACCES;
}
static int nativeProtect(void *address, size_t bytes, int protection) {
    Result rc = svcSetMemoryPermission(address, bytes, (u32)protection);
    if (R_FAILED(rc)) diagnosticsTrace("vm.protect=FAIL address=%p bytes=%zu permission=%d rc=0x%x", address, bytes, protection, rc);
    return R_SUCCEEDED(rc) ? 0 : LINUX_EACCES;
}
static int nativeUnmap(void *destination, void *source, size_t bytes) {
    Result rc = svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), (u64)(uintptr_t)destination, (u64)(uintptr_t)source, bytes);
    if (R_FAILED(rc)) diagnosticsTrace("vm.unmap=FAIL dst=%p src=%p bytes=%zu rc=0x%x", destination, source, bytes, rc);
    return R_SUCCEEDED(rc) ? 0 : LINUX_EIO;
}
static bool nativeCheck(void *address, size_t bytes, int protection, bool mapped, bool data) {
    uintptr_t current = (uintptr_t)address, end = current + bytes;
    while (current < end) {
        MemoryInfo info = {0};
        u32 page;
        Result rc = svcQueryMemory(&info, &page, current);
        u32 state = mapped ? (data ? MemType_ModuleCodeMutable : MemType_ModuleCodeStatic) : MemType_Unmapped;
        bool ok =
            R_SUCCEEDED(rc) && info.addr <= current && info.size > current - info.addr && info.type == state && info.perm == (u32)protection && info.attr == 0;
        if (!ok) {
            diagnosticsTrace("vm.permissions=FAIL address=0x%llx expected=%d actual=%u type=%u mapped=%u rc=0x%x", (unsigned long long)current, protection,
                             info.perm, info.type, mapped, rc);
            return false;
        }
        size_t remaining = info.size - (current - info.addr);
        current += remaining < end - current ? remaining : end - current;
    }
    return true;
}

#define MAX_ARENAS 64u
#define MAX_BACKINGS 4096u
#define CHUNK_PAGES (LINUX_VM_CHUNK / LINUX_VM_PAGE)
// Page metadata is sparse: an arena keeps one pointer per 2 MiB of address
// space. A NULL pointer means "default pages" (owned by the guest, no access,
// no backing), which is what a fresh PROT_NONE reservation looks like, so a
// 32 GiB reservation costs 128 KiB of metadata instead of tens of MiB. HOLE
// marks a span that was unmapped by the guest. Real chunks appear only where
// backing is committed or a range is partially unmapped.
typedef struct {
    uint16_t backing, offset;
    uint8_t active, protection, data;
} Page;
// `bytes` is what the guest was given; `window` the start of it that has real address space (smaller only for a clipped reservation: the
// rest can be unmapped and set to PROT_NONE, never backed).
typedef struct {
    unsigned char *base;
    size_t bytes, active;
    void *reservation;
    Page **chunks;
    size_t window;
} Arena;
typedef struct {
    unsigned char *memory;
    size_t bytes, references;
} Backing;
static Arena arenas[MAX_ARENAS];
static Backing backings[MAX_BACKINGS];
static size_t backing_bytes;
static Page holeChunk[CHUNK_PAGES];
#define HOLE (holeChunk)

static int failure(int error) {
    *linuxAbiErrnoLocation() = error;
    return -1;
}
static bool rounded(size_t bytes, size_t *result) {
    if (!bytes || bytes > SIZE_MAX - (LINUX_VM_PAGE - 1)) return false;
    *result = (bytes + LINUX_VM_PAGE - 1) & ~(size_t)(LINUX_VM_PAGE - 1);
    return true;
}
// Shared writable file mappings are copies; the pages that changed since the last write-back are found by hashing, so no
// shadow copy of the file is needed. Written back by msync, munmap, a new mapping of the same file and at exit.
#define MAX_FILE_MAPPINGS 64u
typedef struct {
    bool used;
    unsigned char *address;
    size_t bytes, pages;
    int64_t offset;
    char path[LINUX_FILE_PATH_MAX];
    uint64_t *hashes;
} FileMapping;
static FileMapping file_mappings[MAX_FILE_MAPPINGS];
static uint64_t pageHash(const unsigned char *page) {
    uint64_t hash = 0xcbf29ce484222325ull, word;
    for (size_t i = 0; i < LINUX_VM_PAGE; i += 8) {
        memcpy(&word, page + i, 8);
        hash = (hash ^ word) * 0x100000001b3ull;
        hash ^= hash >> 29;
    }
    return hash;
}
// 0 or a Linux errno; `release` forgets the mapping afterwards
static int flushMapping(FileMapping *mapping, bool release) {
    int error = 0;
    size_t page = 0;
    while (page < mapping->pages && !error) {
        if (pageHash(mapping->address + page * LINUX_VM_PAGE) == mapping->hashes[page]) {
            ++page;
            continue;
        }
        size_t run = page;
        while (run < mapping->pages && pageHash(mapping->address + run * LINUX_VM_PAGE) != mapping->hashes[run]) ++run;
        size_t from = page * LINUX_VM_PAGE, to = run * LINUX_VM_PAGE;
        if (to > mapping->bytes) to = mapping->bytes;  // the tail of the last page is not part of the file
        if (to > from) error = linuxFilesWriteBack(mapping->path, mapping->offset + (int64_t)from, mapping->address + from, to - from);
        if (!error)
            for (size_t i = page; i < run; ++i) mapping->hashes[i] = pageHash(mapping->address + i * LINUX_VM_PAGE);
        page = run;
    }
    if (release) {
        free(mapping->hashes);
        memset(mapping, 0, sizeof(*mapping));
    }
    return error;
}
static int flushOverlapping(const void *address, size_t bytes, bool release) {
    int error = 0;
    for (unsigned i = 0; i < MAX_FILE_MAPPINGS; ++i) {
        FileMapping *mapping = &file_mappings[i];
        if (!mapping->used) continue;
        uintptr_t low = (uintptr_t)address, high = bytes > UINTPTR_MAX - low ? UINTPTR_MAX : low + bytes;
        uintptr_t start = (uintptr_t)mapping->address, end = start + mapping->pages * LINUX_VM_PAGE;
        if (end <= low || start >= high) continue;
        int result = flushMapping(mapping, release);
        if (result && !error) error = result;
    }
    return error;
}
int linuxVmFlushFileMappings(void) { return flushOverlapping((void *)0, (size_t)-1, false); }
static int protectionError(int protection) {
    if (protection & ~(LINUX_PROT_READ | LINUX_PROT_WRITE | LINUX_PROT_EXEC)) return LINUX_EINVAL;
    if (protection & LINUX_PROT_EXEC) return LINUX_ENOSYS;
    if (protection == LINUX_PROT_WRITE) return LINUX_EACCES;
    return 0;
}
static size_t pageCount(const Arena *arena) { return arena->bytes / LINUX_VM_PAGE; }
static size_t chunkCount(const Arena *arena) { return (pageCount(arena) + CHUNK_PAGES - 1) / CHUNK_PAGES; }
static size_t chunkEnd(size_t index, size_t end) {
    size_t next = (index / CHUNK_PAGES + 1) * CHUNK_PAGES;
    return next < end ? next : end;
}
static bool wholeChunk(const Arena *arena, size_t index, size_t stop) {
    return index % CHUNK_PAGES == 0 && (stop % CHUNK_PAGES == 0 || stop == pageCount(arena));
}
static bool materialized(const Page *chunk) { return chunk && chunk != HOLE; }
static Page pageRead(const Arena *arena, size_t index) {
    const Page *chunk = arena->chunks[index / CHUNK_PAGES];
    if (!chunk) return (Page){.active = 1};
    return chunk[index % CHUNK_PAGES];
}
// Only valid after materialize() covered the page.
static Page *pageRef(Arena *arena, size_t index) { return &arena->chunks[index / CHUNK_PAGES][index % CHUNK_PAGES]; }
static int materialize(Arena *arena, size_t first, size_t end) {
    if (first >= end) return 0;
    for (size_t c = first / CHUNK_PAGES; c <= (end - 1) / CHUNK_PAGES; ++c) {
        Page *chunk = arena->chunks[c];
        if (materialized(chunk)) continue;
        Page *fresh = calloc(CHUNK_PAGES, sizeof(Page));
        if (!fresh) return LINUX_ENOMEM;
        if (!chunk)
            for (size_t i = 0; i < CHUNK_PAGES; ++i) fresh[i].active = 1;
        arena->chunks[c] = fresh;
    }
    return 0;
}
// The real address space of an arena goes first: another arena may sit in the unreserved rest of a clipped one.
static Arena *findArena(void *address, size_t bytes) {
    uintptr_t start = (uintptr_t)address;
    for (int guest_view = 0; guest_view < 2; ++guest_view)
        for (unsigned i = 0; i < MAX_ARENAS; ++i) {
            Arena *arena = &arenas[i];
            size_t extent = guest_view ? arena->bytes : arena->window;
            if (arena->chunks && start >= (uintptr_t)arena->base && start - (uintptr_t)arena->base <= extent &&
                bytes <= extent - (start - (uintptr_t)arena->base))
                return arena;
        }
    return NULL;
}
static void releaseArena(Arena *arena) {
    if (arena->active) return;
    nativeRelease(arena->reservation);
    for (size_t c = 0, count = chunkCount(arena); c < count; ++c)
        if (materialized(arena->chunks[c])) free(arena->chunks[c]);
    free(arena->chunks);
    memset(arena, 0, sizeof(*arena));
}
static size_t mappedRun(Arena *arena, size_t first, size_t end, bool same_protection) {
    Page page = pageRead(arena, first);
    size_t next = first + 1;
    while (next < end) {
        Page other = pageRead(arena, next);
        if (other.backing != page.backing || other.offset != page.offset + next - first ||
            (same_protection && (other.protection != page.protection || other.data != page.data)))
            break;
        ++next;
    }
    return next;
}
static bool allActive(Arena *arena, size_t first, size_t end) {
    for (size_t i = first; i < end;) {
        Page *chunk = arena->chunks[i / CHUNK_PAGES];
        size_t stop = chunkEnd(i, end);
        if (chunk == HOLE) return false;
        if (chunk)
            for (size_t j = i; j < stop; ++j)
                if (!chunk[j % CHUNK_PAGES].active) return false;
        i = stop;
    }
    return true;
}
// Pages in [first,end) that still need backing.
static size_t unbackedPages(Arena *arena, size_t first, size_t end) {
    size_t total = 0;
    for (size_t i = first; i < end;) {
        Page *chunk = arena->chunks[i / CHUNK_PAGES];
        size_t stop = chunkEnd(i, end);
        if (!materialized(chunk))
            total += stop - i;
        else
            for (size_t j = i; j < stop; ++j)
                if (!chunk[j % CHUNK_PAGES].backing) ++total;
        i = stop;
    }
    return total;
}
// Makes every page of the range owned by the guest again (a fixed mapping may
// land on holes left by earlier unmaps).
static int activate(Arena *arena, size_t first, size_t end) {
    for (size_t i = first; i < end;) {
        size_t c = i / CHUNK_PAGES, stop = chunkEnd(i, end);
        Page *chunk = arena->chunks[c];
        if (!chunk) {
            i = stop;
            continue;
        }
        if (chunk == HOLE) {
            if (wholeChunk(arena, i, stop)) {
                arena->chunks[c] = NULL;
                arena->active += stop - i;
                i = stop;
                continue;
            }
            int error = materialize(arena, i, stop);
            if (error) return error;
            chunk = arena->chunks[c];
        }
        for (; i < stop; ++i)
            if (!chunk[i % CHUNK_PAGES].active) {
                chunk[i % CHUNK_PAGES].active = 1;
                ++arena->active;
            }
    }
    return 0;
}
// Removing a successful subrange decrements the backing's references; failed
// unmaps retain both the pages and the allocation for a later retry.
static int drop(Arena *arena, size_t first, size_t end, bool deactivate) {
    for (size_t i = first; i < end;) {
        Page *chunk = arena->chunks[i / CHUNK_PAGES];
        if (!materialized(chunk)) {
            size_t stop = chunkEnd(i, end);
            if (deactivate && !chunk) {
                if (wholeChunk(arena, i, stop)) {
                    arena->chunks[i / CHUNK_PAGES] = HOLE;
                    arena->active -= stop - i;
                } else {
                    int error = materialize(arena, i, stop);
                    if (error) return error;
                    continue;
                }
            }
            i = stop;
            continue;
        }
        Page page = chunk[i % CHUNK_PAGES];
        size_t next = i + 1;
        if (page.backing) {
            next = mappedRun(arena, i, end, false);
            Backing *backing = &backings[page.backing - 1];
            int error = nativeUnmap(arena->base + i * LINUX_VM_PAGE, backing->memory + page.offset * LINUX_VM_PAGE, (next - i) * LINUX_VM_PAGE);
            if (error) return error;
            backing->references -= next - i;
            if (!backing->references) {
                nativeFree(backing->memory);
                backing_bytes -= backing->bytes;
                memset(backing, 0, sizeof(*backing));
            }
        }
        for (; i < next; ++i) {
            Page *entry = pageRef(arena, i);
            if (deactivate && entry->active) --arena->active;
            *entry = (Page){.active = deactivate ? 0 : entry->active};
        }
    }
    return 0;
}
static int commit(Arena *arena, size_t first, size_t end) {
    // Refuse before touching anything when the range cannot possibly be backed.
    if (end > arena->window / LINUX_VM_PAGE) return LINUX_ENOMEM;
    if (unbackedPages(arena, first, end) > (LINUX_VM_MAX_BACKING - backing_bytes) / LINUX_VM_PAGE) return LINUX_ENOMEM;
    int error = materialize(arena, first, end);
    if (error) return error;
    while (first < end) {
        if (pageRef(arena, first)->backing) {
            ++first;
            continue;
        }
        size_t next = first + 1;
        while (next < end && !pageRef(arena, next)->backing && next - first < CHUNK_PAGES) ++next;
        unsigned index = 0;
        while (index < MAX_BACKINGS && backings[index].memory) ++index;
        size_t bytes = (next - first) * LINUX_VM_PAGE;
        if (index == MAX_BACKINGS || bytes > LINUX_VM_MAX_BACKING - backing_bytes) return LINUX_ENOMEM;
        unsigned char *source = nativeAllocate(bytes);
        if (!source) return LINUX_ENOMEM;
        // The source becomes inaccessible when MapProcessCodeMemory succeeds.
        memset(source, 0, bytes);
        error = nativeMap(arena->base + first * LINUX_VM_PAGE, source, bytes);
        if (error) {
            nativeFree(source);
            return error;
        }
        backings[index] = (Backing){source, bytes, next - first};
        backing_bytes += bytes;
        for (size_t i = first; i < next; ++i) {
            Page *page = pageRef(arena, i);
            page->backing = index + 1;
            page->offset = i - first;
            page->protection = LINUX_PROT_NONE;
            page->data = 0;
        }
        // Metadata already owns these pages if conversion/protection fails.
        first = next;
    }
    return 0;
}
static int protect(Arena *arena, size_t first, size_t end, int protection) {
    int error = protection ? commit(arena, first, end) : 0;
    if (error) return error;
    for (size_t i = first; i < end;) {
        Page *chunk = arena->chunks[i / CHUNK_PAGES];
        if (!materialized(chunk)) {
            i = chunkEnd(i, end);
            continue;
        }
        Page *page = pageRef(arena, i);
        if (!page->backing) {
            ++i;
            continue;
        }
        size_t next = mappedRun(arena, i, end, true);
        void *address = arena->base + i * LINUX_VM_PAGE;
        size_t bytes = (next - i) * LINUX_VM_PAGE;
        if (!page->data && protection) {
            error = nativeConvert(address, bytes);
            if (error) return error;
            for (size_t j = i; j < next; ++j) {
                pageRef(arena, j)->data = 1;
                pageRef(arena, j)->protection = 3;
            }
        }
        if (page->protection != protection) {
            error = nativeProtect(address, bytes, protection);
            if (error) return error;
            for (size_t j = i; j < next; ++j) pageRef(arena, j)->protection = protection;
        }
        if (!nativeCheck(address, bytes, protection, true, page->data)) return LINUX_EIO;
        i = next;
    }
    return 0;
}
static void *mapAnonymous(void *address, size_t bytes, int protection, int flags, int64_t offset, int *result_error) {
    size_t size = 0;
    int error = protectionError(protection);
    if (!error && (!rounded(bytes, &size) || offset < 0 || (uint64_t)offset % LINUX_VM_PAGE)) error = LINUX_EINVAL;
    if (!error && ((flags & (LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS)) != (LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS) ||
                   (flags & ~(LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | LINUX_MAP_FIXED | LINUX_MAP_NORESERVE))))
        error = LINUX_ENOSYS;
    if (!error && size > LINUX_VM_MAX_RESERVATION) error = LINUX_ENOMEM;
    if (!error && (flags & LINUX_MAP_FIXED) && (!address || (uintptr_t)address % LINUX_VM_PAGE)) error = LINUX_EINVAL;
    if (error) {
        *result_error = error;
        return LINUX_MAP_FAILED;
    }
    lockVm();
    Arena *arena = NULL;
    bool created = false;
    if (!backendAvailable())
        error = LINUX_ENOSYS;
    else if (flags & LINUX_MAP_FIXED) {
        arena = findArena(address, size);
        if (!arena) error = LINUX_ENOSYS;  // Only owned reservations may be replaced.
    } else {
        for (unsigned i = 0; i < MAX_ARENAS; ++i)
            if (!arenas[i].chunks) {
                arena = &arenas[i];
                break;
            }
        if (!arena)
            error = LINUX_ENOMEM;
        else {
            size_t pages = size / LINUX_VM_PAGE;
            Page **chunks = calloc((pages + CHUNK_PAGES - 1) / CHUNK_PAGES, sizeof(Page *));
            void *base = NULL, *reservation = NULL;
            size_t window = 0;
            error = chunks ? nativeReserve(size, &base, &reservation, &window) : LINUX_ENOMEM;
            if (error)
                free(chunks);
            else {
                *arena = (Arena){base, size, pages, reservation, chunks, window};
                address = base;
                created = true;
            }
        }
    }
    if (!error) {
        size_t first = ((uintptr_t)address - (uintptr_t)arena->base) / LINUX_VM_PAGE;
        size_t end = first + size / LINUX_VM_PAGE;
        if (!created) {
            error = drop(arena, first, end, false);
            if (!error) error = activate(arena, first, end);
        }
        if (!error) error = protect(arena, first, end, protection);
        if (error && created) {
            // Failed cleanup retains the object; releaseAll can retry safely.
            drop(arena, 0, pageCount(arena), true);
            releaseArena(arena);
        }
    }
    unlockVm();
    *result_error = error;
    return error ? LINUX_MAP_FAILED : address;
}
// Only shared READ-ONLY file mappings exist (FileChannel.map(READ_ONLY): the game's data archives). The mapping is a
// copy of the bytes taken when it is made, so later changes of the file are not seen. Writable and private file
// mappings stay refused: their writes could not be stored back, and silently losing data is worse than an error.
static void *mapFile(void *address, size_t bytes, int protection, int flags, int fd, int64_t offset, int *result_error) {
    size_t size = 0;
    int error = protectionError(protection);
    bool writable = (protection & LINUX_PROT_WRITE) != 0;
    if (!error && (protection == LINUX_PROT_WRITE || (flags & (LINUX_MAP_SHARED | LINUX_MAP_PRIVATE)) != LINUX_MAP_SHARED)) error = LINUX_ENOSYS;
    if (!error && (!rounded(bytes, &size) || offset < 0 || (uint64_t)offset % LINUX_VM_PAGE)) error = LINUX_EINVAL;
    if (!error && (flags & ~(LINUX_MAP_SHARED | LINUX_MAP_PRIVATE | LINUX_MAP_NORESERVE))) error = LINUX_ENOSYS;  // no MAP_FIXED over a file
    if (!error && address && (flags & LINUX_MAP_FIXED)) error = LINUX_ENOSYS;
    if (error) {
        *result_error = error;
        return LINUX_MAP_FAILED;
    }
    char path[LINUX_FILE_PATH_MAX];
    int descriptor_flags = 0;
    if (writable) {
        if (!linuxFilesDescriptorInfo(fd, path, &descriptor_flags)) {
            *result_error = *linuxAbiErrnoLocation() ? *linuxAbiErrnoLocation() : LINUX_EBADF;
            return LINUX_MAP_FAILED;
        }
        if ((descriptor_flags & 3) != 2) {
            *result_error = 13;
            return LINUX_MAP_FAILED;
        }                                           // a shared writable mapping needs a descriptor open for reading and writing
        flushOverlapping(NULL, (size_t)-1, false);  // other mappings of this file must reach the file before it is read again
    }
    void *memory =
        mapAnonymous(NULL, bytes, LINUX_PROT_READ | LINUX_PROT_WRITE, LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | (flags & LINUX_MAP_NORESERVE), 0, &error);
    if (error) {
        *result_error = error;
        return LINUX_MAP_FAILED;
    }
    size_t done = 0;
    while (done < bytes && !error) {
        size_t chunk = bytes - done < (1u << 20) ? bytes - done : (1u << 20);
        int64_t count = linuxAbiPread(fd, (unsigned char *)memory + done, chunk, offset + (int64_t)done);
        if (count < 0)
            error = *linuxAbiErrnoLocation() ? *linuxAbiErrnoLocation() : LINUX_EIO;
        else if (count == 0)
            break;  // past the end of the file: the rest stays zero, as on Linux
        else
            done += (size_t)count;
    }
    if (!error && !writable && linuxAbiMprotect(memory, size, LINUX_PROT_READ)) error = *linuxAbiErrnoLocation();
    FileMapping *tracked = NULL;
    if (!error && writable) {
        for (unsigned i = 0; i < MAX_FILE_MAPPINGS && !tracked; ++i)
            if (!file_mappings[i].used) tracked = &file_mappings[i];
        uint64_t *hashes = tracked ? calloc(size / LINUX_VM_PAGE, sizeof(uint64_t)) : NULL;
        if (!tracked || !hashes) {
            free(hashes);
            error = LINUX_ENOMEM;
            tracked = NULL;
        } else {
            for (size_t i = 0; i < size / LINUX_VM_PAGE; ++i) hashes[i] = pageHash((unsigned char *)memory + i * LINUX_VM_PAGE);
            *tracked = (FileMapping){.used = true, .address = memory, .bytes = bytes, .pages = size / LINUX_VM_PAGE, .offset = offset, .hashes = hashes};
            strcpy(tracked->path, path);
        }
    }
    if (error) {
        linuxAbiMunmap(memory, size);
        *result_error = error;
        return LINUX_MAP_FAILED;
    }
    *result_error = 0;
    return memory;
}
void *linuxAbiMmap(void *address, size_t bytes, int protection, int flags, int fd, int64_t offset) {
    int error = 0;
    void *result = (flags & LINUX_MAP_ANONYMOUS) ? mapAnonymous(address, bytes, protection, flags, offset, &error)  // fd is ignored
                                                 : mapFile(address, bytes, protection, flags, fd, offset, &error);
    if (error) failure(error);
    if (error) diagnosticsTrace("vm.mmap=ERROR address=%p bytes=%zu prot=%d flags=0x%x fd=%d errno=%d", address, bytes, protection, flags, fd, error);
    return result;
}
int linuxAbiMprotect(void *address, size_t bytes, int protection) {
    size_t size = 0;
    int error = protectionError(protection);
    if (!error && (uintptr_t)address % LINUX_VM_PAGE) error = LINUX_EINVAL;
    if (!error && !bytes) return 0;
    if (!error && !rounded(bytes, &size)) error = LINUX_EINVAL;
    if (!error) {
        lockVm();
        Arena *arena = findArena(address, size);
        if (!arena)
            error = LINUX_ENOMEM;
        else {
            size_t first = ((uintptr_t)address - (uintptr_t)arena->base) / LINUX_VM_PAGE;
            size_t end = first + size / LINUX_VM_PAGE;
            if (!allActive(arena, first, end))
                error = LINUX_ENOMEM;
            else
                error = protect(arena, first, end, protection);
        }
        unlockVm();
    }
    if (error) diagnosticsTrace("vm.mprotect=ERROR address=%p bytes=%zu prot=%d errno=%d", address, bytes, protection, error);
    return error ? failure(error) : 0;
}
int linuxAbiMsync(void *address, size_t bytes, int flags) {
    enum { MS_ASYNC = 1, MS_INVALIDATE = 2, MS_SYNC = 4 };
    if ((uintptr_t)address % LINUX_VM_PAGE || (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) || ((flags & MS_ASYNC) && (flags & MS_SYNC)))
        return failure(LINUX_EINVAL);
    int error = flushOverlapping(address, bytes ? bytes : 1, false);
    return error ? failure(error) : 0;
}
int linuxAbiMunmap(void *address, size_t bytes) {
    size_t size = 0;
    int error = LINUX_EINVAL;
    if (!((uintptr_t)address % LINUX_VM_PAGE) && rounded(bytes, &size)) {
        int write_back = flushOverlapping(address, size, true);  // shared writable file mappings reach their file first
        if (write_back) diagnosticsTrace("vm.munmap.write_back=ERROR address=%p errno=%d", address, write_back);
        lockVm();
        Arena *arena = findArena(address, size);
        error = LINUX_ENOSYS;
        if (arena) {
            size_t first = ((uintptr_t)address - (uintptr_t)arena->base) / LINUX_VM_PAGE;
            error = drop(arena, first, first + size / LINUX_VM_PAGE, true);
            if (!error) releaseArena(arena);
        }
        unlockVm();
    }
    if (error) diagnosticsTrace("vm.munmap=ERROR address=%p bytes=%zu errno=%d", address, bytes, error);
    return error ? failure(error) : 0;
}
bool linuxVmReleaseAll(void) {
    linuxVmFlushFileMappings();
    for (unsigned i = 0; i < MAX_FILE_MAPPINGS; ++i)
        if (file_mappings[i].used) {
            free(file_mappings[i].hashes);
            memset(&file_mappings[i], 0, sizeof(file_mappings[i]));
        }
    lockVm();
    bool ok = true;
    for (unsigned i = 0; i < MAX_ARENAS; ++i)
        if (arenas[i].chunks) {
            if (drop(&arenas[i], 0, pageCount(&arenas[i]), true)) ok = false;
            releaseArena(&arenas[i]);
        }
    unlockVm();
    return ok;
}
LinuxVmStats linuxVmStats(void) {
    lockVm();
    LinuxVmStats result = {.backing_bytes = backing_bytes};
    for (unsigned i = 0; i < MAX_ARENAS; ++i)
        if (arenas[i].chunks) {
            ++result.arenas;
            result.active_pages += arenas[i].active;
        }
    for (unsigned i = 0; i < MAX_BACKINGS; ++i) result.mapped_pages += backings[i].references;
    unlockVm();
    return result;
}
