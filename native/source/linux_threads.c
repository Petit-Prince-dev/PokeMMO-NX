#include "linux_threads.h"
#include "linux_tls.h"
#include "linux_abi.h"
#include "linux_runtime.h"
#include "diagnostics.h"
#include <stdlib.h>
#include <string.h>
#include <switch.h>

typedef struct {
    Thread thread;
    bool created;
    size_t guard;
    VirtmemReservation *reservation;
} NativeThread;
static Mutex records_lock;
static uint32_t nativeHandle(void *token) { return token ? ((NativeThread *)token)->thread.handle : 0; }
static void lockRecords(void) { mutexLock(&records_lock); }
static void unlockRecords(void) { mutexUnlock(&records_lock); }
// Horizon time-slices only the threads of priority 59 (0x3b) on the application cores: threads of any other priority run until they
// block, so a thread that computes for ever starves every other thread of its priority on that core (a spinning audio thread did, and a thread
// holding a lock never ran again). Guest threads therefore run at 0x3b, below the application's own main thread (0x2c).
// The first guest thread is the game's main thread, which renders: it gets a core of its own, and every other thread (sound decoding, network,
// caches) shares the remaining cores, so that no background work takes time from a frame.
static unsigned ownedCores(unsigned available[3]) {
    static u64 mask;
    if (!mask) {
        u64 value = 0;
        mask = R_SUCCEEDED(svcGetInfo(&value, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && (value & 7) ? (value & 7) : 7;
    }
    unsigned count = 0;
    for (unsigned core = 0; core < 3; ++core)
        if (mask & (1u << core)) available[count++] = core;
    return count;
}
static int nextCore(void) {
    static unsigned counter;
    unsigned available[3], count = ownedCores(available);
    unsigned n = __atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED);
    if (n == 0 || count == 1) return (int)available[0];
    return (int)available[1 + (n - 1) % (count - 1)];
}
int linuxThreadBackgroundCore(void) {
    unsigned available[3], count = ownedCores(available);
    return (int)available[count - 1];
}
static int nativeCreate(void **token, void (*entry)(void *), void *argument, size_t stack, size_t guard) {
    NativeThread *native = calloc(1, sizeof(*native));
    if (!native) return LINUX_EAGAIN;
    *token = native;
    int core = nextCore();
    Result rc = threadCreate(&native->thread, entry, argument, NULL, stack, 0x3b, core);
    if (R_FAILED(rc)) rc = threadCreate(&native->thread, entry, argument, NULL, stack, 0x2c, -2);
    if (R_FAILED(rc)) {
        diagnosticsTrace("pthread.native.create=FAIL rc=0x%x stack_request=%zu guard_request=%zu core=%d", rc, stack, guard, core);
        return LINUX_EAGAIN;
    }
    native->created = true;
    if (guard) {
        uintptr_t base = (uintptr_t)native->thread.stack_mirror;
        MemoryInfo info = {0};
        u32 page;
        virtmemLock();
        rc = base >= guard ? svcQueryMemory(&info, &page, base - guard) : MAKERESULT(Module_Libnx, LibnxError_BadInput);
        bool available = R_SUCCEEDED(rc) && info.type == MemType_Unmapped && info.perm == 0 && info.addr <= base - guard && info.size >= base - info.addr;
        if (available) native->reservation = virtmemAddReservation((void *)(base - guard), guard);
        virtmemUnlock();
        if (!native->reservation) {
            diagnosticsTrace("pthread.native.guard=FAIL base=0x%llx bytes=%zu query_rc=0x%x", (unsigned long long)(base - guard), guard, rc);
            return LINUX_EAGAIN;
        }
        native->guard = guard;
    }
    return 0;
}
static int nativeStart(void *token) {
    NativeThread *native = token;
    Result rc = threadStart(&native->thread);
    if (R_FAILED(rc)) diagnosticsTrace("pthread.native.start=FAIL rc=0x%x", rc);
    return R_SUCCEEDED(rc) ? 0 : LINUX_EAGAIN;
}
static int nativeJoin(void *token) {
    Result rc = threadWaitForExit(&((NativeThread *)token)->thread);
    if (R_FAILED(rc)) diagnosticsTrace("pthread.native.join=FAIL rc=0x%x", rc);
    return R_SUCCEEDED(rc) ? 0 : LINUX_EIO;
}
static int nativeClose(void *token) {
    NativeThread *native = token;
    Result rc = native->created ? threadClose(&native->thread) : 0;
    if (R_FAILED(rc)) {
        diagnosticsTrace("pthread.native.close=FAIL retained=YES rc=0x%x", rc);
        return LINUX_EIO;
    }
    if (native->reservation) {
        virtmemLock();
        virtmemRemoveReservation(native->reservation);
        virtmemUnlock();
    }
    free(native);
    return 0;
}
static int stackInfo(Thread *thread, size_t guard, void **base, size_t *bytes, size_t *guard_out) {
    if (!thread || !thread->stack_mirror || !thread->stack_sz) return LINUX_EIO;
    MemoryInfo info = {0};
    u32 page;
    Result rc = svcQueryMemory(&info, &page, (u64)(uintptr_t)thread->stack_mirror);
    bool ok = R_SUCCEEDED(rc) && info.perm == Perm_Rw && info.addr <= (uintptr_t)thread->stack_mirror &&
              info.size >= (uintptr_t)thread->stack_mirror - info.addr && thread->stack_sz <= info.size - ((uintptr_t)thread->stack_mirror - info.addr);
    if (ok && guard) {
        uintptr_t bottom = (uintptr_t)thread->stack_mirror - guard;
        rc = svcQueryMemory(&info, &page, bottom);
        ok = R_SUCCEEDED(rc) && info.type == MemType_Unmapped && info.perm == 0 && info.addr <= bottom &&
             info.size >= (uintptr_t)thread->stack_mirror - info.addr;
    }
    if (!ok) {
        diagnosticsTrace("pthread.native.stack=FAIL base=%p usable=%zu guard=%zu query_rc=0x%x", thread->stack_mirror, thread->stack_sz, guard, rc);
        return LINUX_EIO;
    }
    *base = (unsigned char *)thread->stack_mirror - guard;
    *bytes = thread->stack_sz + guard;
    *guard_out = guard;
    return 0;
}
static int nativeStack(void *token, void **base, size_t *bytes, size_t *guard) {
    NativeThread *native = token;
    return native && native->created ? stackInfo(&native->thread, native->guard, base, bytes, guard) : LINUX_EIO;
}
static int nativeCurrentStack(void **base, size_t *bytes, size_t *guard) { return stackInfo(threadGetSelf(), 0, base, bytes, guard); }
void linuxThreadYield(void) { svcSleepThread(0); }
_Static_assert(sizeof(LinuxPthreadAttr) == 64 && sizeof(LinuxPthread) == 8 && sizeof(LinuxPthreadKey) == 4, "AArch64 glibc pthread layouts");
// glibc LP64 attr offsets: flags 8, guard 16, stack-top 24, size 32,
// extension 40. Never reinterpret an attr as a libnx/newlib object.
enum { ATTR_FLAGS = 8, ATTR_GUARD = 16, ATTR_TOP = 24, ATTR_SIZE = 32 };
typedef struct {
    LinuxPthread id;
    void *native;
    LinuxThreadEntry entry;
    void *argument, *result;
    bool started, exited, claimed, detached;
} Record;
typedef struct {
    uint64_t generation;
    bool used;
    void (*destructor)(void *);
} Key;
typedef struct {
    uint64_t generation;
    void *value;
} Value;
static Record records[LINUX_THREAD_MAX_THREADS];
static Key keys[LINUX_THREAD_MAX_KEYS];
static _Thread_local Value values[LINUX_THREAD_MAX_KEYS];
static _Thread_local LinuxPthread self_id;
static LinuxPthread next_id = 1;
static bool shutting_down;
static size_t created;
static uint64_t read64(const LinuxPthreadAttr *attr, unsigned offset) {
    uint64_t value;
    memcpy(&value, (const unsigned char *)attr + offset, sizeof(value));
    return value;
}
static void write64(LinuxPthreadAttr *attr, unsigned offset, uint64_t value) { memcpy((unsigned char *)attr + offset, &value, sizeof(value)); }
static uint32_t flags(const LinuxPthreadAttr *attr) {
    uint32_t result;
    memcpy(&result, (const unsigned char *)attr + ATTR_FLAGS, sizeof(result));
    return result;
}
static Record *find(LinuxPthread id) {
    for (unsigned i = 0; i < LINUX_THREAD_MAX_THREADS; ++i)
        if (records[i].id == id && id) return &records[i];
    return NULL;
}
static LinuxPthread newId(void) { return next_id && next_id != UINT64_MAX ? next_id++ : 0; }
int linuxPthreadAttrInit(LinuxPthreadAttr *attr) {
    if (!attr) return LINUX_EINVAL;
    memset(attr, 0, sizeof(*attr));
    write64(attr, ATTR_GUARD, 4096);
    write64(attr, ATTR_SIZE, LINUX_THREAD_DEFAULT_STACK);
    return 0;
}
int linuxPthreadAttrDestroy(LinuxPthreadAttr *attr) {
    if (!attr) return LINUX_EINVAL;
    if (read64(attr, 40)) return LINUX_ENOSYS;
    memset(attr, 0, sizeof(*attr));
    return 0;
}
int linuxPthreadAttrSetDetachState(LinuxPthreadAttr *attr, int state) {
    if (!attr || (state != 0 && state != 1)) return LINUX_EINVAL;
    if (state == 1) return LINUX_ENOSYS;
    uint32_t value = flags(attr) & ~1u;
    memcpy((unsigned char *)attr + ATTR_FLAGS, &value, sizeof(value));
    return 0;
}
int linuxPthreadAttrSetStackSize(LinuxPthreadAttr *attr, size_t bytes) {
    if (!attr || bytes < 16384 || bytes > SIZE_MAX - 4095) return LINUX_EINVAL;
    if (bytes > LINUX_THREAD_MAX_STACK) return LINUX_ENOSYS;
    write64(attr, ATTR_SIZE, bytes);
    return 0;
}
int linuxPthreadAttrGetStack(const LinuxPthreadAttr *attr, void **base, size_t *bytes) {
    if (!attr || !base || !bytes) return LINUX_EINVAL;
    uint64_t top = read64(attr, ATTR_TOP), size = read64(attr, ATTR_SIZE);
    if (top && top < size) return LINUX_EINVAL;
    *base = top ? (void *)(uintptr_t)(top - size) : NULL;
    *bytes = size;
    return 0;
}
int linuxPthreadAttrGetGuardSize(const LinuxPthreadAttr *attr, size_t *bytes) {
    if (!attr || !bytes) return LINUX_EINVAL;
    *bytes = read64(attr, ATTR_GUARD);
    return 0;
}
LinuxPthread linuxPthreadSelf(void) {
    if (!self_id) {
        lockRecords();
        self_id = newId();
        unlockRecords();
    }
    return self_id;
}
bool linuxPthreadManaged(LinuxPthread thread) {
    if (!thread) return false;
    lockRecords();
    bool managed = find(thread) != NULL;
    unlockRecords();
    return managed;
}
bool linuxPthreadExists(LinuxPthread thread) { return thread && (thread == linuxPthreadSelf() || linuxPthreadManaged(thread)); }
int linuxPthreadGetattr(LinuxPthread thread, LinuxPthreadAttr *attr) {
    if (!attr) return LINUX_EINVAL;
    LinuxPthread self = linuxPthreadSelf();
    lockRecords();
    Record *record = find(thread);
    void *base = NULL;
    size_t size = 0, guard = 0;
    int error = record ? nativeStack(record->native, &base, &size, &guard) : (thread == self ? nativeCurrentStack(&base, &size, &guard) : LINUX_ESRCH);
    if (!error) {
        memset(attr, 0, sizeof(*attr));
        write64(attr, ATTR_TOP, (uintptr_t)base + size);
        write64(attr, ATTR_SIZE, size);
        write64(attr, ATTR_GUARD, guard);
        uint32_t value = 8;
        memcpy((unsigned char *)attr + ATTR_FLAGS, &value, sizeof(value));
    }
    unlockRecords();
    return error;
}
static void destructValues(void) {
    for (unsigned pass = 0; pass < 4; ++pass) {
        bool called = false;
        for (unsigned i = 0; i < LINUX_THREAD_MAX_KEYS; ++i) {
            lockRecords();
            void *value = values[i].value;
            void (*destructor)(void *) = NULL;
            if (keys[i].used && values[i].generation == keys[i].generation && value) destructor = keys[i].destructor;
            values[i].value = NULL;
            unlockRecords();
            if (destructor) {
                called = true;
                destructor(value);
            }
        }
        if (!called) break;
    }
    memset(values, 0, sizeof(values));
}
static void entryWrapper(void *argument) {
    Record *record = argument;
    self_id = record->id;
    linuxTlsAttachThread();  // thread-local storage for loaded libraries; a failure leaves the thread pointer at zero
    // exit()/abort() inside the entry unwind to here: the thread still runs its
    // TLS destructors and publishes a NULL result, so it can be joined normally.
    void *result = NULL;
    linuxRuntimeRun(record->entry, record->argument, &result);
    destructValues();
    linuxTlsDetachThread();
    lockRecords();
    record->result = result;
    record->exited = true;
    unlockRecords();
}
// Detached threads have no joiner: once they ended, the creator of the next thread joins and frees them.
static void reapDetached(void) {
    for (;;) {
        void *native = NULL;
        Record *record = NULL;
        lockRecords();
        for (unsigned i = 0; i < LINUX_THREAD_MAX_THREADS; ++i)
            if (records[i].id && records[i].detached && records[i].exited && records[i].started && !records[i].claimed) {
                record = &records[i];
                record->claimed = true;
                native = record->native;
                break;
            }
        unlockRecords();
        if (!record) return;
        int error = nativeJoin(native);
        if (!error) error = nativeClose(native);
        lockRecords();
        if (!error)
            memset(record, 0, sizeof(*record));
        else
            record->claimed = false;
        unlockRecords();
        if (error) return;
    }
}
int linuxPthreadDetach(LinuxPthread thread) {
    lockRecords();
    Record *record = find(thread);
    int error = !record ? LINUX_ESRCH : ((!record->started || record->claimed || record->detached) ? LINUX_EINVAL : 0);
    if (!error) record->detached = true;
    unlockRecords();
    return error;
}
int linuxPthreadCreate(LinuxPthread *thread, const LinuxPthreadAttr *attr, LinuxThreadEntry entry, void *argument) {
    if (!thread || !entry) return LINUX_EINVAL;
    reapDetached();
    LinuxPthreadAttr defaults;
    if (!attr) {
        linuxPthreadAttrInit(&defaults);
        attr = &defaults;
    }
    if (flags(attr) || read64(attr, 0) || read64(attr, 40) || read64(attr, 48) || read64(attr, ATTR_TOP)) return LINUX_ENOSYS;
    size_t stack = read64(attr, ATTR_SIZE), guard = read64(attr, ATTR_GUARD);
    if (!stack) stack = LINUX_THREAD_DEFAULT_STACK;
    if (stack < 16384 || stack > SIZE_MAX - 4095) return LINUX_EINVAL;
    if (stack > LINUX_THREAD_MAX_STACK || (guard != 0 && guard != 4096)) return LINUX_ENOSYS;
    stack = (stack + 4095) & ~(size_t)4095;
    lockRecords();
    Record *record = NULL;
    if (!shutting_down)
        for (unsigned i = 0; i < LINUX_THREAD_MAX_THREADS; ++i)
            if (!records[i].id) {
                record = &records[i];
                break;
            }
    int error = LINUX_EAGAIN;
    LinuxPthread id = record ? newId() : 0;
    if (id) {
        *record = (Record){.id = id, .entry = entry, .argument = argument};
        error = nativeCreate(&record->native, entryWrapper, record, stack, guard);
        if (!error) {
            error = nativeStart(record->native);
            if (!error) {
                record->started = true;
                *thread = id;
                ++created;
            }
        }
        if (error && (!record->native || !nativeClose(record->native))) memset(record, 0, sizeof(*record));
    }
    unlockRecords();
    return error;
}
int linuxPthreadJoin(LinuxPthread thread, void **result) {
    if (thread == linuxPthreadSelf()) return LINUX_EDEADLK;
    lockRecords();
    Record *record = find(thread);
    int error = !record ? LINUX_ESRCH : ((!record->started || record->claimed || record->detached) ? LINUX_EINVAL : 0);
    if (error) {
        unlockRecords();
        return error;
    }
    record->claimed = true;
    void *native = record->native;
    unlockRecords();
    error = nativeJoin(native);
    lockRecords();
    if (!error) error = nativeClose(native);
    if (!error) {
        if (result) *result = record->result;
        memset(record, 0, sizeof(*record));
    } else
        record->claimed = false;
    unlockRecords();
    return error;
}
int linuxPthreadKeyCreate(LinuxPthreadKey *key, void (*destructor)(void *)) {
    if (!key) return LINUX_EINVAL;
    lockRecords();
    int error = LINUX_EAGAIN;
    for (unsigned i = 0; i < LINUX_THREAD_MAX_KEYS; ++i)
        if (!keys[i].used && keys[i].generation != UINT64_MAX) {
            ++keys[i].generation;
            keys[i].used = true;
            keys[i].destructor = destructor;
            *key = i;
            error = 0;
            break;
        }
    unlockRecords();
    return error;
}
int linuxPthreadKeyDelete(LinuxPthreadKey key) {
    lockRecords();
    int error = key >= LINUX_THREAD_MAX_KEYS || !keys[key].used ? LINUX_EINVAL : 0;
    if (!error) {
        keys[key].used = false;
        keys[key].destructor = NULL;
    }
    unlockRecords();
    return error;
}
void *linuxPthreadGetSpecific(LinuxPthreadKey key) {
    lockRecords();
    void *value = key < LINUX_THREAD_MAX_KEYS && keys[key].used && values[key].generation == keys[key].generation ? values[key].value : NULL;
    unlockRecords();
    return value;
}
int linuxPthreadSetSpecific(LinuxPthreadKey key, const void *value) {
    lockRecords();
    int error = key >= LINUX_THREAD_MAX_KEYS || !keys[key].used ? LINUX_EINVAL : 0;
    if (!error) values[key] = (Value){keys[key].generation, (void *)value};
    unlockRecords();
    return error;
}
int linuxSchedYield(void) {
    linuxThreadYield();
    return 0;
}
unsigned linuxThreadsLiveUnlocked(void) {
    unsigned live = 0;
    for (unsigned i = 0; i < LINUX_THREAD_MAX_THREADS; ++i) {
        volatile const Record *record = &records[i];
        if (record->id && record->started && !record->exited) ++live;
    }
    return live;
}
unsigned linuxThreadsSnapshot(LinuxThreadSnapshot *out, unsigned max) {
    unsigned count = 0;
    for (unsigned i = 0; i < LINUX_THREAD_MAX_THREADS && count < max; ++i) {
        volatile const Record *record = &records[i];
        if (!record->id || !record->started || record->exited || !record->native) continue;
        out[count++] = (LinuxThreadSnapshot){record->id, nativeHandle(record->native), true, false};
    }
    return count;
}
LinuxThreadStats linuxThreadsStats(void) {
    lockRecords();
    LinuxThreadStats stats = {.created = created};
    for (unsigned i = 0; i < LINUX_THREAD_MAX_THREADS; ++i)
        if (records[i].id) {
            ++stats.threads;
            stats.live_threads += records[i].started && !records[i].exited;
        }
    unlockRecords();
    return stats;
}
bool linuxThreadsReset(void) {
    LinuxPthread self = linuxPthreadSelf();
    lockRecords();
    if (shutting_down) {
        unlockRecords();
        return false;
    }
    shutting_down = true;
    bool ok = true;
    for (unsigned i = 0; i < LINUX_THREAD_MAX_THREADS; ++i)
        if (records[i].id) {
            Record *record = &records[i];
            if (record->claimed || record->id == self) {
                ok = false;
                continue;
            }
            record->claimed = true;
            void *native = record->native;
            bool started = record->started;
            unlockRecords();
            int error = started ? nativeJoin(native) : 0;
            lockRecords();
            if (!error) error = nativeClose(native);
            if (error) {
                record->claimed = false;
                ok = false;
            } else
                memset(record, 0, sizeof(*record));
        }
    if (ok) {
        for (unsigned i = 0; i < LINUX_THREAD_MAX_KEYS; ++i) {
            keys[i].used = false;
            keys[i].destructor = NULL;
        }
        memset(values, 0, sizeof(values));
        created = 0;
    }
    shutting_down = false;
    unlockRecords();
    return ok;
}
