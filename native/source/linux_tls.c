#include "linux_tls.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t offset;
    size_t file_bytes, memory_bytes;
    unsigned char *image;
} Module;
static Module modules[LINUX_TLS_MAX_MODULES];
static unsigned module_count;
static uint64_t next_offset = LINUX_TLS_FIRST_OFFSET;
static unsigned char *arenas[LINUX_TLS_MAX_THREADS];
static atomic_flag guard = ATOMIC_FLAG_INIT;
static void lock(void) {
    while (atomic_flag_test_and_set_explicit(&guard, memory_order_acquire)) {}
}
static void unlock(void) { atomic_flag_clear_explicit(&guard, memory_order_release); }

// TPIDR_EL0 is free for user code on Horizon (libnx keeps its thread variables behind TPIDRRO_EL0 and the compiler runs with -mtp=soft), so it
// carries the thread pointer of the guest libraries. The TLS descriptor resolver (asm) is linuxTlsDescResolver.
static void setThreadPointer(void *pointer) { __asm__ volatile("msr tpidr_el0, %0" : : "r"(pointer) : "memory"); }

static void fill(unsigned char *arena, const Module *module) {
    memcpy(arena + module->offset, module->image, module->file_bytes);
    memset(arena + module->offset + module->file_bytes, 0, module->memory_bytes - module->file_bytes);
}

bool linuxTlsRegisterModule(const void *image, size_t file_bytes, size_t memory_bytes, size_t alignment, uint64_t *offset) {
    if (!offset || file_bytes > memory_bytes || (file_bytes && !image)) return false;
    if (alignment < 16) alignment = 16;
    if (alignment & (alignment - 1)) return false;
    lock();
    uint64_t start = (next_offset + alignment - 1) & ~(uint64_t)(alignment - 1);
    bool ok = module_count < LINUX_TLS_MAX_MODULES && memory_bytes <= LINUX_TLS_ARENA_BYTES && start <= LINUX_TLS_ARENA_BYTES - memory_bytes;
    unsigned char *copy = NULL;
    if (ok && file_bytes) {
        copy = malloc(file_bytes);
        ok = copy != NULL;
        if (copy) memcpy(copy, image, file_bytes);
    }
    if (ok) {
        Module *module = &modules[module_count++];
        *module = (Module){start, file_bytes, memory_bytes, copy};
        next_offset = start + memory_bytes;
        for (unsigned i = 0; i < LINUX_TLS_MAX_THREADS; ++i)
            if (arenas[i]) fill(arenas[i], module);
        *offset = start;
    }
    unlock();
    return ok;
}

static _Thread_local unsigned char *current;
bool linuxTlsAttachThread(void) {
    unsigned char *arena = calloc(1, LINUX_TLS_ARENA_BYTES);
    if (!arena) return false;
    lock();
    unsigned slot = 0;
    while (slot < LINUX_TLS_MAX_THREADS && arenas[slot]) ++slot;
    bool ok = slot < LINUX_TLS_MAX_THREADS;
    if (ok) {
        arenas[slot] = arena;
        for (unsigned i = 0; i < module_count; ++i) fill(arena, &modules[i]);
    }
    unlock();
    if (!ok) {
        free(arena);
        return false;
    }
    current = arena;
    setThreadPointer(arena);
    return true;
}

void linuxTlsDetachThread(void) {
    unsigned char *arena = current;
    if (!arena) return;
    current = NULL;
    setThreadPointer(NULL);
    lock();
    for (unsigned i = 0; i < LINUX_TLS_MAX_THREADS; ++i)
        if (arenas[i] == arena) arenas[i] = NULL;
    unlock();
    free(arena);
}

bool linuxTlsReset(void) {
    lock();
    bool ok = true;
    for (unsigned i = 0; i < LINUX_TLS_MAX_THREADS; ++i)
        if (arenas[i]) ok = false;
    if (ok) {
        for (unsigned i = 0; i < module_count; ++i) free(modules[i].image);
        memset(modules, 0, sizeof(modules));
        module_count = 0;
        next_offset = LINUX_TLS_FIRST_OFFSET;
    }
    unlock();
    return ok;
}

unsigned linuxTlsModuleCount(void) {
    lock();
    unsigned count = module_count;
    unlock();
    return count;
}
