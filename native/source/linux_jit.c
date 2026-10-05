#include "linux_jit.h"
#include "diagnostics.h"
#include <stdarg.h>
#include <string.h>
#include <switch.h>

#define POOL_BYTES (256u * 1024u)
#define SLOT_ALIGN 64u

static Jit pool;
static bool created;
static size_t used;
static RMutex lock;
static unsigned allocations, preparations;
typedef int (*PrepClosure)(void *closure, void *cif, void *function, void *user_data, void *code);
typedef int (*PrepClosureJni)(void *env, void *clazz, intptr_t closure, intptr_t cif, intptr_t function, intptr_t user_data, intptr_t code);
static PrepClosure original_prepare;
static PrepClosureJni original_prepare_jni;

// The pool is kept executable; it is writable only while libffi writes a trampoline.
static bool ensurePool(void) {
    if (created) return true;
    Result rc = jitCreate(&pool, POOL_BYTES);
    diagnosticsTrace("jit.create=%s rc=0x%x type=%d size=%u rw=%p rx=%p executable=%d hint_create=%d hint_control=%d", R_SUCCEEDED(rc) ? "PASS" : "FAIL", rc,
                     pool.type, POOL_BYTES, pool.rw_addr, pool.rx_addr, pool.is_executable, envIsSyscallHinted(0x4B), envIsSyscallHinted(0x4C));
    if (R_FAILED(rc)) return false;
    if (!pool.is_executable) {
        rc = jitTransitionToExecutable(&pool);
        diagnosticsTrace("jit.executable=%s rc=0x%x", R_SUCCEEDED(rc) ? "PASS" : "FAIL", rc);
        if (R_FAILED(rc)) {
            jitClose(&pool);
            return false;
        }
    }
    created = true;
    return true;
}

static void *closureAlloc(size_t size, void **code) {
    rmutexLock(&lock);
    void *writable = NULL;
    if (size && code && size <= POOL_BYTES && ensurePool()) {
        size_t slot = (size + SLOT_ALIGN - 1) & ~(size_t)(SLOT_ALIGN - 1);
        if (used + slot <= POOL_BYTES) {
            writable = (char *)jitGetRwAddr(&pool) + used;
            *code = (char *)jitGetRxAddr(&pool) + used;
            used += slot;
            ++allocations;
            if (allocations <= 8) diagnosticsTrace("jit.alloc size=%zu writable=%p code=%p", size, writable, *code);
        }
    }
    if (!writable) diagnosticsTrace("jit.alloc=FAIL size=%zu used=%zu", size, used);
    rmutexUnlock(&lock);
    return writable;
}

static void closureFree(void *closure) { (void)closure; }

static int prepareClosure(void *closure, void *cif, void *function, void *user_data, void *code) {
    enum { FFI_BAD_ABI = 2 };
    rmutexLock(&lock);
    int status = FFI_BAD_ABI;
    if (created && original_prepare) {
        Result rc = jitTransitionToWritable(&pool);
        if (R_SUCCEEDED(rc)) {
            status = original_prepare(closure, cif, function, user_data, code);
            armDCacheFlush(pool.rw_addr, POOL_BYTES);
            rc = jitTransitionToExecutable(&pool);
            armICacheInvalidate(pool.rx_addr, POOL_BYTES);
            if (R_FAILED(rc)) {
                status = FFI_BAD_ABI;
                diagnosticsTrace("jit.executable=FAIL rc=0x%x", rc);
            }
        } else
            diagnosticsTrace("jit.writable=FAIL rc=0x%x", rc);
        ++preparations;
        if (preparations <= 8) diagnosticsTrace("jit.prepare status=%d closure=%p code=%p", status, closure, code);
    }
    rmutexUnlock(&lock);
    return status;
}

// LWJGL links libffi statically and does not export it: its Java side reaches libffi through these JNI entry points,
// which the VM finds by name, so they are replaced the same way (the JNI arguments are (env, class, ...)).
static intptr_t closureAllocJni(void *env, void *clazz, size_t size, void **code) {
    (void)env;
    (void)clazz;
    return (intptr_t)closureAlloc(size, code);
}
static void closureFreeJni(void *env, void *clazz, intptr_t closure) {
    (void)env;
    (void)clazz;
    (void)closure;
}
static int prepareClosureJni(void *env, void *clazz, intptr_t closure, intptr_t cif, intptr_t function, intptr_t user_data, intptr_t code) {
    enum { FFI_BAD_ABI = 2 };
    rmutexLock(&lock);
    int status = FFI_BAD_ABI;
    if (created && original_prepare_jni) {
        Result rc = jitTransitionToWritable(&pool);
        if (R_SUCCEEDED(rc)) {
            status = original_prepare_jni(env, clazz, closure, cif, function, user_data, code);
            armDCacheFlush(pool.rw_addr, POOL_BYTES);
            rc = jitTransitionToExecutable(&pool);
            armICacheInvalidate(pool.rx_addr, POOL_BYTES);
            if (R_FAILED(rc)) {
                status = FFI_BAD_ABI;
                diagnosticsTrace("jit.executable=FAIL rc=0x%x", rc);
            }
        } else
            diagnosticsTrace("jit.writable=FAIL rc=0x%x", rc);
        ++preparations;
        if (preparations <= 8) diagnosticsTrace("jit.prepare status=%d closure=%p code=%p (jni)", status, (void *)closure, (void *)code);
    }
    rmutexUnlock(&lock);
    return status;
}

uintptr_t linuxJitOverride(const char *name, uintptr_t original) {
    if (!name || !original) return original;
    if (strstr(name, "closure") && (!strncmp(name, "ffi_", 4) || !strncmp(name, "Java_org_lwjgl_system_libffi_", 29)))
        diagnosticsTrace("jit.override=%s", name);
    if (!strcmp(name, "ffi_closure_alloc")) return (uintptr_t)closureAlloc;
    if (!strcmp(name, "ffi_closure_free")) return (uintptr_t)closureFree;
    if (!strcmp(name, "ffi_prep_closure_loc")) {
        original_prepare = (PrepClosure)original;
        return (uintptr_t)prepareClosure;
    }
    if (!strcmp(name, "Java_org_lwjgl_system_libffi_LibFFI_nffi_1closure_1alloc")) return (uintptr_t)closureAllocJni;
    if (!strcmp(name, "Java_org_lwjgl_system_libffi_LibFFI_nffi_1closure_1free")) return (uintptr_t)closureFreeJni;
    if (!strcmp(name, "Java_org_lwjgl_system_libffi_LibFFI_nffi_1prep_1closure_1loc")) {
        original_prepare_jni = (PrepClosureJni)original;
        return (uintptr_t)prepareClosureJni;
    }
    return original;
}

void linuxJitReset(void) {
    rmutexLock(&lock);
    if (created) {
        Result rc = jitClose(&pool);
        diagnosticsTrace("jit.close=%s rc=0x%x", R_SUCCEEDED(rc) ? "PASS" : "FAIL", rc);
    }
    created = false;
    used = 0;
    original_prepare = NULL;
    original_prepare_jni = NULL;
    allocations = preparations = 0;
    rmutexUnlock(&lock);
}
