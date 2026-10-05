#include "linux_trap.h"
#include "diagnostics.h"
#include "linux_runtime.h"
#include <elf.h>
#include <stdatomic.h>
#include <string.h>

typedef struct {
    char name[96], version[24];
    _Atomic unsigned calls;
    _Atomic uintptr_t first_caller;
} Trap;
static Trap traps[LINUX_TRAP_MAX];
static unsigned used;
static _Atomic unsigned requests;
static _Atomic uint64_t refused;
static atomic_flag lock_flag = ATOMIC_FLAG_INIT;
static void lock(void) {
    while (atomic_flag_test_and_set_explicit(&lock_flag, memory_order_acquire)) linuxThreadYield();
}
static void unlock(void) { atomic_flag_clear_explicit(&lock_flag, memory_order_release); }

bool linuxTrapResolve(void *context, const ElfImport *symbol, uintptr_t *address) {
    if (linuxAbiResolve(context, symbol, address)) return true;
    // Weak imports must stay null (the program tests them); objects cannot be stubbed.
    if (symbol->type != STT_FUNC || symbol->binding == STB_WEAK || !symbol->name) return false;
    lock();
    unsigned index = LINUX_TRAP_MAX;
    for (unsigned i = 0; i < used; ++i)
        if (!strcmp(traps[i].name, symbol->name) && !strcmp(traps[i].version, symbol->version ? symbol->version : "")) {
            index = i;
            break;
        }
    if (index == LINUX_TRAP_MAX && used < LINUX_TRAP_MAX && strlen(symbol->name) < sizeof(traps[0].name)) {
        index = used++;
        strcpy(traps[index].name, symbol->name);
        if (symbol->version && strlen(symbol->version) < sizeof(traps[index].version)) strcpy(traps[index].version, symbol->version);
    }
    unlock();
    if (index == LINUX_TRAP_MAX) return false;  // table full: stays unresolved, never silently accepted
    *address = (uintptr_t)linuxTrapStubs + (uintptr_t)index * LINUX_TRAP_STUB_BYTES;
    return true;
}
const char *linuxTrapName(unsigned index) { return index < used ? traps[index].name : NULL; }
unsigned linuxTrapCalls(unsigned index) { return index < used ? atomic_load_explicit(&traps[index].calls, memory_order_relaxed) : 0; }
uintptr_t linuxTrapFirstCaller(unsigned index) { return index < used ? atomic_load_explicit(&traps[index].first_caller, memory_order_relaxed) : 0; }
void linuxTrapReset(void) {
    lock();
    for (unsigned i = 0; i < LINUX_TRAP_MAX; ++i) {
        traps[i].name[0] = 0;
        traps[i].version[0] = 0;
        atomic_store_explicit(&traps[i].calls, 0, memory_order_relaxed);
        atomic_store_explicit(&traps[i].first_caller, 0, memory_order_relaxed);
    }
    used = 0;
    atomic_store_explicit(&requests, 0, memory_order_relaxed);
    atomic_store_explicit(&refused, 0, memory_order_relaxed);
    unlock();
}
LinuxTrapStats linuxTrapStats(void) {
    lock();
    LinuxTrapStats stats = {used, atomic_load_explicit(&requests, memory_order_relaxed), atomic_load_explicit(&refused, memory_order_relaxed)};
    unlock();
    return stats;
}
static const char *linuxTrapVersion(unsigned index) { return index < used ? traps[index].version : NULL; }
static int64_t linuxTrapRefusalValue(const char *name) {
    // Functions whose failure value is not -1: pointers (NULL), sizes and counts (0).
    static const char *const zero[] = {"strftime",     "localtime_r",   "gmtime_r",   "setmntent",  "getmntent_r",
                                       "gai_strerror", "__ctype_b_loc", "getpwnam_r", "getgrnam_r", "getgrgid_r",
                                       "freeaddrinfo", "endmntent",     "rewind",     "times",      NULL};
    for (unsigned i = 0; zero[i]; ++i)
        if (!strcmp(name, zero[i])) return 0;
    return -1;
}
int64_t linuxTrapReport(unsigned index, uintptr_t caller) {
    atomic_fetch_add_explicit(&requests, 1, memory_order_relaxed);
    const char *name = linuxTrapName(index), *version = linuxTrapVersion(index);
    unsigned calls = 0;
    if (index < LINUX_TRAP_MAX) {
        calls = atomic_fetch_add_explicit(&traps[index].calls, 1, memory_order_relaxed) + 1;
        if (calls == 1) atomic_store_explicit(&traps[index].first_caller, caller, memory_order_relaxed);
    }
    bool refuse = name && atomic_load_explicit(&refused, memory_order_relaxed) < LINUX_TRAP_REFUSAL_LIMIT;
    // Log the first call of every import and then the calls that are powers of two.
    if (!refuse || calls == 1 || !(calls & (calls - 1)))
        diagnosticsTrace("client.trap=%s index=%u name=%s version=%s caller=0x%llx calls=%u", refuse ? "REFUSED" : "UNRESOLVED_IMPORT", index,
                         name ? name : "?", version && *version ? version : "NONE", (unsigned long long)caller, calls);
    if (!refuse) linuxRuntimeUnresolvedImport(index);
    atomic_fetch_add_explicit(&refused, 1, memory_order_relaxed);
    *linuxAbiErrnoLocation() = LINUX_ENOSYS;
    return linuxTrapRefusalValue(name);
}
