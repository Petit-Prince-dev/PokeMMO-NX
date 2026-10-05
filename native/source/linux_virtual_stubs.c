#include "linux_virtual_stubs.h"
#include "diagnostics.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// The stubs themselves are the table of linux_virtual_stub_table.s (one per slot); a slot is given to each name asked for.
#define POOL 1024u
#define STUB_BYTES 8u
extern const unsigned char linuxVirtualStubTable[];
static char *stub_names[POOL];
static atomic_uint stub_calls[POOL];
static unsigned stub_used;
static atomic_flag stub_lock = ATOMIC_FLAG_INIT;

// Called by the stubs (assembly): remembers the first call of each, which the diagnostics file lists.
uintptr_t linuxVirtualStubCalled(unsigned slot) {
    if (slot < POOL && atomic_fetch_add(&stub_calls[slot], 1) == 0 && stub_names[slot])
        diagnosticsTrace("virtual.stub.called=%s slot=%u", stub_names[slot], slot);
    return 0;
}

static void lockPool(void) {
    while (atomic_flag_test_and_set_explicit(&stub_lock, memory_order_acquire)) {}
}
static void unlockPool(void) { atomic_flag_clear_explicit(&stub_lock, memory_order_release); }

uintptr_t linuxVirtualStub(const char *name) {
    if (!name || !*name) return 0;
    unsigned slot = POOL;
    lockPool();
    for (unsigned i = 0; i < stub_used && slot == POOL; ++i)
        if (!strcmp(stub_names[i], name)) slot = i;
    if (slot == POOL && stub_used < POOL) {
        size_t length = strlen(name) + 1;
        char *copy = malloc(length);
        if (copy) {
            memcpy(copy, name, length);
            stub_names[stub_used] = copy;
            slot = stub_used++;
        }
    }
    unlockPool();
    return slot == POOL ? 0 : (uintptr_t)linuxVirtualStubTable + (uintptr_t)slot * STUB_BYTES;
}
void linuxVirtualStubsReset(void) {
    lockPool();
    for (unsigned i = 0; i < stub_used; ++i) {
        free(stub_names[i]);
        stub_names[i] = NULL;
        atomic_store(&stub_calls[i], 0);
    }
    stub_used = 0;
    unlockPool();
}
