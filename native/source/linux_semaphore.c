#include "linux_semaphore.h"
#include "linux_sync.h"
#include "diagnostics.h"
#include <string.h>
#include <switch.h>
_Static_assert(sizeof(LinuxSemaphore) == 32 && _Alignof(LinuxSemaphore) == 8, "glibc LP64 semaphore layout");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_POINTER_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2, "lock-free semaphore operations required");
#define CLOSED UINT32_C(0x80000000)
#define WAIT_UNIT (UINT64_C(1) << 32)
typedef struct {
    _Atomic(LinuxSemaphore *) object;
    _Atomic uint32_t state;
} Record;
static Record records[LINUX_SEM_MAX_OBJECTS];
static int fail(int error) {
    *linuxAbiErrnoLocation() = error;
    return -1;
}

// The kernel's address arbiter does the blocking: wait while the word is zero, wake on a post.
static bool backendAvailable(void) { return envIsSyscallHinted(0x1a) && envIsSyscallHinted(0x1b) && envIsSyscallHinted(0x34) && envIsSyscallHinted(0x35); }
static int nativeWait(_Atomic uint64_t *data, uint64_t timeout) {
    Result rc = svcWaitForAddress(data, ArbitrationType_WaitIfEqual, 0, timeout);
    int error = R_SUCCEEDED(rc)         ? 0
                : R_VALUE(rc) == 0xfa01 ? LINUX_EAGAIN
                : R_VALUE(rc) == 0xea01 ? LINUX_ETIMEDOUT
                : R_VALUE(rc) == 0xec01 ? LINUX_EINTR
                                        : LINUX_EIO;
    if (error == LINUX_EIO) diagnosticsTrace("sem.native.wait=FAIL timeout_ns=%llu rc=0x%x", (unsigned long long)timeout, rc);
    return error;
}
// sem_post may run in a signal handler. Keep this path free of log locks, heap allocation and mutexes; only the kernel wake and lock-free counters.
static int nativeWake(_Atomic uint64_t *data) {
    Result rc = svcSignalToAddress(data, SignalType_Signal, 0, 1);
    return R_SUCCEEDED(rc) ? 0 : LINUX_EIO;
}
static bool valid(const void *object) { return object && !((uintptr_t)object & 7); }
static void release(Record *record) { atomic_fetch_sub_explicit(&record->state, 1, memory_order_release); }
static bool pin(Record *record) {
    uint32_t state = atomic_load_explicit(&record->state, memory_order_relaxed);
    while (!(state & CLOSED) && state < CLOSED - 1)
        if (atomic_compare_exchange_weak_explicit(&record->state, &state, state + 1, memory_order_acquire, memory_order_relaxed)) return true;
    return false;
}
static Record *hold(LinuxSemaphore *object) {
    if (!valid(object)) return NULL;
    for (unsigned i = 0; i < LINUX_SEM_MAX_OBJECTS; ++i) {
        Record *record = &records[i];
        if (atomic_load_explicit(&record->object, memory_order_acquire) != object || !pin(record)) continue;
        if (atomic_load_explicit(&record->object, memory_order_acquire) == object) return record;
        release(record);
    }
    return NULL;
}
int linuxSemInit(LinuxSemaphore *object, int shared, unsigned value) {
    if (!valid(object) || value > LINUX_SEM_VALUE_MAX) return fail(LINUX_EINVAL);
    if (shared) return fail(LINUX_ENOSYS);
    if (!backendAvailable()) return fail(LINUX_ENOSYS);
    linuxSyncLock();
    Record *empty = NULL;
    for (unsigned i = 0; i < LINUX_SEM_MAX_OBJECTS; ++i) {
        LinuxSemaphore *existing = atomic_load_explicit(&records[i].object, memory_order_acquire);
        if (existing == object) {
            linuxSyncUnlock();
            return fail(LINUX_EBUSY);
        }
        if (!existing && !empty) empty = &records[i];
    }
    if (!empty) {
        linuxSyncUnlock();
        return fail(LINUX_EAGAIN);
    }
    atomic_store_explicit(&empty->state, CLOSED, memory_order_relaxed);
    memset(object, 0, sizeof(*object));
    atomic_init(&object->data, value);  // low32 tokens, high32 waiters; private=0
    atomic_store_explicit(&empty->object, object, memory_order_release);
    atomic_store_explicit(&empty->state, 0, memory_order_release);
    linuxSyncUnlock();
    return 0;
}
int linuxSemDestroy(LinuxSemaphore *object) {
    if (!valid(object)) return fail(LINUX_EINVAL);
    linuxSyncLock();
    Record *found = NULL;
    for (unsigned i = 0; i < LINUX_SEM_MAX_OBJECTS; ++i)
        if (atomic_load_explicit(&records[i].object, memory_order_acquire) == object) {
            found = &records[i];
            break;
        }
    if (!found) {
        linuxSyncUnlock();
        return fail(LINUX_EINVAL);
    }
    uint32_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&found->state, &expected, CLOSED, memory_order_acquire, memory_order_relaxed)) {
        linuxSyncUnlock();
        return fail(LINUX_EBUSY);
    }
    atomic_store_explicit(&found->object, NULL, memory_order_release);
    atomic_store_explicit(&object->data, UINT64_MAX, memory_order_relaxed);
    linuxSyncUnlock();
    return 0;
}
int linuxSemPost(LinuxSemaphore *object) {
    Record *record = hold(object);
    if (!record) return fail(LINUX_EINVAL);
    uint64_t data = atomic_load_explicit(&object->data, memory_order_relaxed);
    do {
        if ((uint32_t)data >= LINUX_SEM_VALUE_MAX) {
            release(record);
            return fail(LINUX_EOVERFLOW);
        }
    } while (!atomic_compare_exchange_weak_explicit(&object->data, &data, data + 1, memory_order_release, memory_order_relaxed));
    // Pin through the SVC. An unexpected wake error retains the published
    // token: a waiter may have consumed it already. Polling bounds recovery.
    int error = data >> 32 ? nativeWake(&object->data) : 0;
    release(record);
    return error ? fail(error) : 0;
}
int linuxSemWait(LinuxSemaphore *object) {
    Record *record = hold(object);
    if (!record) return fail(LINUX_EINVAL);
    uint64_t data = atomic_load_explicit(&object->data, memory_order_relaxed);
    while ((uint32_t)data)
        if (atomic_compare_exchange_weak_explicit(&object->data, &data, data - 1, memory_order_acquire, memory_order_relaxed)) {
            release(record);
            return 0;
        }
    atomic_fetch_add_explicit(&object->data, WAIT_UNIT, memory_order_relaxed);
    int error = 0;
    for (;;) {
        data = atomic_load_explicit(&object->data, memory_order_relaxed);
        if ((uint32_t)data) {
            if (atomic_compare_exchange_weak_explicit(&object->data, &data, data - 1 - WAIT_UNIT, memory_order_acquire, memory_order_relaxed)) break;
        } else {
            error = nativeWait(&object->data, LINUX_SEM_POLL_NS);
            if (error == 0 || error == LINUX_EAGAIN || error == LINUX_ETIMEDOUT) {
                error = 0;
                continue;
            }
            atomic_fetch_sub_explicit(&object->data, WAIT_UNIT, memory_order_relaxed);
            break;
        }
    }
    release(record);
    return error ? fail(error) : 0;
}
bool linuxSemReset(void) {
    bool claimed[LINUX_SEM_MAX_OBJECTS] = {0};
    linuxSyncLock();
    for (unsigned i = 0; i < LINUX_SEM_MAX_OBJECTS; ++i) {
        if (!atomic_load_explicit(&records[i].object, memory_order_acquire)) continue;
        uint32_t expected = 0;
        if (!atomic_compare_exchange_strong_explicit(&records[i].state, &expected, CLOSED, memory_order_acquire, memory_order_relaxed)) {
            for (unsigned j = 0; j < i; ++j)
                if (claimed[j]) atomic_store_explicit(&records[j].state, 0, memory_order_release);
            linuxSyncUnlock();
            return false;
        }
        claimed[i] = true;
    }
    // Objects may refer to expired stack frames. Forget their addresses only.
    for (unsigned i = 0; i < LINUX_SEM_MAX_OBJECTS; ++i)
        if (claimed[i]) atomic_store_explicit(&records[i].object, NULL, memory_order_release);
    linuxSyncUnlock();
    return true;
}
