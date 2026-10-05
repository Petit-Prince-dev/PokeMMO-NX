#include "linux_sync.h"
#include "linux_threads.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
_Static_assert(sizeof(LinuxMutex) == 48 && sizeof(LinuxCondition) == 48 && sizeof(LinuxMutexAttr) == 8 && sizeof(LinuxConditionAttr) == 8,
               "glibc AArch64 LP64 sync sizes");
// The kernel objects behind the Linux ones: libnx mutexes and condition variables (futex based, no kernel handle).
typedef struct {
    Mutex mutex;
} NativeMutex;
typedef struct {
    CondVar condition;
} NativeCond;
static Mutex registry;
void linuxSyncLock(void) { mutexLock(&registry); }
void linuxSyncUnlock(void) { mutexUnlock(&registry); }
static bool backendAvailable(void) { return envIsSyscallHinted(0x1a) && envIsSyscallHinted(0x1b) && envIsSyscallHinted(0x1c) && envIsSyscallHinted(0x1d); }
static int nativeMutexCreate(void **token) {
    NativeMutex *native = calloc(1, sizeof(*native));
    if (!native) return LINUX_ENOMEM;
    mutexInit(&native->mutex);
    *token = native;
    return 0;
}
static int nativeMutexDestroy(void *token) {
    free(token);
    return 0;
}
static int nativeMutexLock(void *token) {
    mutexLock(&((NativeMutex *)token)->mutex);
    return 0;
}
static int nativeMutexTryLock(void *token) { return mutexTryLock(&((NativeMutex *)token)->mutex) ? 0 : LINUX_EBUSY; }
static int nativeMutexUnlock(void *token) {
    mutexUnlock(&((NativeMutex *)token)->mutex);
    return 0;
}
static int nativeCondCreate(void **token) {
    NativeCond *native = calloc(1, sizeof(*native));
    if (!native) return LINUX_ENOMEM;
    condvarInit(&native->condition);
    *token = native;
    return 0;
}
static int nativeCondDestroy(void *token) {
    free(token);
    return 0;
}
// Always returns with the mutex reacquired, including errors and timeouts.
static int nativeWait(void *condition, void *mutex, uint64_t timeout) {
    NativeMutex *lock = mutex;
    Result rc = condvarWaitTimeout(&((NativeCond *)condition)->condition, &lock->mutex, timeout);
    // libnx reacquires on timeout. On any other failure, ensure the promised ownership before returning an error/spurious wake to the Linux caller.
    if (!mutexIsLockedByCurrentThread(&lock->mutex)) mutexLock(&lock->mutex);
    return R_SUCCEEDED(rc) || R_VALUE(rc) == 0xec01 ? 0 : (R_VALUE(rc) == 0xea01 ? LINUX_ETIMEDOUT : LINUX_EIO);
}
static int nativeWake(void *condition, bool all) {
    Result rc = all ? condvarWakeAll(&((NativeCond *)condition)->condition) : condvarWakeOne(&((NativeCond *)condition)->condition);
    return R_SUCCEEDED(rc) ? 0 : LINUX_EIO;
}
typedef struct {
    LinuxMutex *object;
    void *native;
    LinuxPthread owner;
    unsigned depth, type, references;
} MutexRecord;
typedef struct {
    LinuxCondition *object;
    void *native;
    MutexRecord *bound;
    unsigned waiters;
    int clock;
} CondRecord;
static MutexRecord mutexes[LINUX_SYNC_MAX_MUTEXES];
static CondRecord conditions[LINUX_SYNC_MAX_CONDITIONS];
static bool aligned(const void *pointer) { return pointer && !((uintptr_t)pointer & 7); }
static uint32_t read32(const void *object, unsigned offset) {
    uint32_t value;
    memcpy(&value, (const unsigned char *)object + offset, sizeof(value));
    return value;
}
static void write32(void *object, unsigned offset, uint32_t value) { memcpy((unsigned char *)object + offset, &value, sizeof(value)); }
static bool zeroExcept(const void *object, unsigned offset) {
    const unsigned char *bytes = object;
    for (unsigned i = 0; i < 48; ++i)
        if ((i < offset || i >= offset + 4) && bytes[i]) return false;
    return true;
}
static MutexRecord *findMutex(LinuxMutex *object) {
    for (unsigned i = 0; i < LINUX_SYNC_MAX_MUTEXES; ++i)
        if (mutexes[i].object == object) return &mutexes[i];
    return NULL;
}
static CondRecord *findCond(LinuxCondition *object) {
    for (unsigned i = 0; i < LINUX_SYNC_MAX_CONDITIONS; ++i)
        if (conditions[i].object == object) return &conditions[i];
    return NULL;
}
static int newMutex(LinuxMutex *object, unsigned type, MutexRecord **output) {
    if (!backendAvailable()) return LINUX_ENOSYS;
    for (unsigned i = 0; i < LINUX_SYNC_MAX_MUTEXES; ++i)
        if (!mutexes[i].object) {
            void *native = NULL;
            int error = nativeMutexCreate(&native);
            if (error) return error;
            mutexes[i] = (MutexRecord){.object = object, .native = native, .type = type};
            *output = &mutexes[i];
            return 0;
        }
    return LINUX_EAGAIN;
}
static int newCond(LinuxCondition *object, int clock, CondRecord **output) {
    if (!backendAvailable()) return LINUX_ENOSYS;
    for (unsigned i = 0; i < LINUX_SYNC_MAX_CONDITIONS; ++i)
        if (!conditions[i].object) {
            void *native = NULL;
            int error = nativeCondCreate(&native);
            if (error) return error;
            conditions[i] = (CondRecord){.object = object, .native = native, .clock = clock};
            *output = &conditions[i];
            return 0;
        }
    return LINUX_EAGAIN;
}
static int getMutex(LinuxMutex *object, MutexRecord **output) {
    if (!aligned(object)) return LINUX_EINVAL;
    *output = findMutex(object);
    if (*output) return 0;
    // Recognize glibc's static initializers, including its kind at offset 16.
    uint32_t type = read32(object, 16);
    if (type == UINT32_MAX || !zeroExcept(object, 16)) return LINUX_EINVAL;
    if (type > 2) return LINUX_ENOSYS;
    return newMutex(object, type, output);
}
static int getCond(LinuxCondition *object, CondRecord **output) {
    if (!aligned(object)) return LINUX_EINVAL;
    *output = findCond(object);
    if (*output) return 0;
    // glibc __wrefs: shared bit 0, MONOTONIC bit 1, offset 36.
    uint32_t flags = read32(object, 36);
    if (flags == UINT32_MAX || !zeroExcept(object, 36)) return LINUX_EINVAL;
    if (flags & ~2u) return LINUX_ENOSYS;
    return newCond(object, (flags >> 1) & 1, output);
}
int linuxPthreadMutexInit(LinuxMutex *object, const LinuxMutexAttr *attr) {
    if (!aligned(object)) return LINUX_EINVAL;
    uint32_t type = attr ? read32(attr, 0) : 0;
    if (type > 2) return LINUX_ENOSYS;  // no adaptive, robust, PI or shared mutex
    linuxSyncLock();
    MutexRecord *record;
    int error = findMutex(object) ? LINUX_EBUSY : newMutex(object, type, &record);
    if (!error) {
        memset(object, 0, sizeof(*object));
        write32(object, 16, type);
    }
    linuxSyncUnlock();
    return error;
}
int linuxPthreadMutexDestroy(LinuxMutex *object) {
    if (!aligned(object)) return LINUX_EINVAL;
    linuxSyncLock();
    MutexRecord *record;
    int error = getMutex(object, &record);
    if (!error && (record->owner || record->references)) error = LINUX_EBUSY;
    if (!error) error = nativeMutexDestroy(record->native);
    if (!error) {
        memset(record, 0, sizeof(*record));
        memset(object, 0, sizeof(*object));
        write32(object, 16, UINT32_MAX);
    }
    linuxSyncUnlock();
    return error;
}
static int lockMutex(LinuxMutex *object, bool attempt) {
    LinuxPthread self = linuxPthreadSelf();
    linuxSyncLock();
    MutexRecord *record;
    int error = getMutex(object, &record);
    if (!error && record->owner == self) {
        if (record->type == 1) {
            error = record->depth == UINT_MAX ? LINUX_EAGAIN : 0;
            if (!error) ++record->depth;
        } else {
            error = attempt ? LINUX_EBUSY : LINUX_EDEADLK;
        }
        linuxSyncUnlock();
        return error;
    }
    if (error) {
        linuxSyncUnlock();
        return error;
    }
    ++record->references;
    void *native = record->native;
    linuxSyncUnlock();
    error = attempt ? nativeMutexTryLock(native) : nativeMutexLock(native);
    linuxSyncLock();
    --record->references;
    if (!error) {
        record->owner = self;
        record->depth = 1;
    }
    linuxSyncUnlock();
    return error;
}
int linuxPthreadMutexLock(LinuxMutex *object) { return lockMutex(object, false); }
int linuxPthreadMutexTryLock(LinuxMutex *object) { return lockMutex(object, true); }
int linuxPthreadMutexUnlock(LinuxMutex *object) {
    LinuxPthread self = linuxPthreadSelf();
    linuxSyncLock();
    MutexRecord *record;
    int error = getMutex(object, &record);
    if (!error && record->owner != self) error = LINUX_EPERM;
    if (!error) {
        if (record->depth == 1) error = nativeMutexUnlock(record->native);
        if (!error) {
            if (!--record->depth) record->owner = 0;
        }
    }
    linuxSyncUnlock();
    return error;
}
int linuxPthreadCondAttrInit(LinuxConditionAttr *attr) {
    if (!attr) return LINUX_EINVAL;
    memset(attr, 0, sizeof(*attr));
    return 0;
}
int linuxPthreadCondAttrDestroy(LinuxConditionAttr *attr) {
    if (!attr) return LINUX_EINVAL;
    memset(attr, 0, sizeof(*attr));
    return 0;
}
int linuxPthreadCondAttrSetClock(LinuxConditionAttr *attr, int clock) {
    if (!attr || (clock != 0 && clock != 1)) return LINUX_EINVAL;
    uint32_t flags = read32(attr, 0);
    if (flags & ~3u) return LINUX_EINVAL;
    write32(attr, 0, (flags & ~2u) | ((uint32_t)clock << 1));
    return 0;
}
int linuxPthreadCondInit(LinuxCondition *object, const LinuxConditionAttr *attr) {
    if (!aligned(object)) return LINUX_EINVAL;
    uint32_t flags = attr ? read32(attr, 0) : 0;
    if (flags & ~2u) return LINUX_ENOSYS;
    linuxSyncLock();
    CondRecord *record;
    int error = findCond(object) ? LINUX_EBUSY : newCond(object, (flags >> 1) & 1, &record);
    if (!error) {
        memset(object, 0, sizeof(*object));
        write32(object, 36, flags);
    }
    linuxSyncUnlock();
    return error;
}
int linuxPthreadCondDestroy(LinuxCondition *object) {
    if (!aligned(object)) return LINUX_EINVAL;
    linuxSyncLock();
    CondRecord *record;
    int error = getCond(object, &record);
    if (!error && record->waiters) error = LINUX_EBUSY;
    if (!error) error = nativeCondDestroy(record->native);
    if (!error) {
        memset(record, 0, sizeof(*record));
        memset(object, 0, sizeof(*object));
        write32(object, 36, UINT32_MAX);
    }
    linuxSyncUnlock();
    return error;
}
static int remaining(int clock, const LinuxTimespec *deadline, uint64_t *timeout) {
    if (!deadline) {
        *timeout = UINT64_MAX;
        return 0;
    }
    LinuxTimespec now;
    int error = linuxAbiReadClock(clock, &now);  // pthread errors must not touch Linux errno
    if (error) return error;
    if (now.nanoseconds < 0 || now.nanoseconds >= 1000000000) return LINUX_EIO;
    __int128 delta = ((__int128)deadline->seconds - now.seconds) * 1000000000 + deadline->nanoseconds - now.nanoseconds;
    *timeout = delta <= 0 ? 0 : (delta > INT64_MAX ? INT64_MAX : (uint64_t)delta);
    return 0;
}
static int waitCondition(LinuxCondition *object, LinuxMutex *mutex, const LinuxTimespec *deadline) {
    if (deadline && (deadline->nanoseconds < 0 || deadline->nanoseconds >= 1000000000)) return LINUX_EINVAL;
    LinuxPthread self = linuxPthreadSelf();
    linuxSyncLock();
    CondRecord *condition;
    MutexRecord *lock;
    int error = getCond(object, &condition);
    if (!error) error = getMutex(mutex, &lock);
    if (!error && lock->owner != self) error = LINUX_EPERM;
    if (!error && lock->depth != 1) error = LINUX_EINVAL;
    if (!error && condition->waiters && condition->bound != lock) error = LINUX_EINVAL;
    uint64_t timeout = 0;
    if (!error) error = remaining(condition->clock, deadline, &timeout);
    if (error) {
        linuxSyncUnlock();
        return error;
    }
    ++lock->references;
    ++condition->waiters;
    condition->bound = lock;
    lock->owner = 0;
    lock->depth = 0;
    linuxSyncUnlock();
    do {
        // Resample REALTIME at most every second so a wall-clock adjustment
        // cannot turn an absolute wait into an unbounded relative delay.
        uint64_t slice = deadline && condition->clock == 0 && timeout > 1000000000 ? 1000000000 : timeout;
        error = nativeWait(condition->native, lock->native, slice);
        if (error != LINUX_ETIMEDOUT || !deadline) break;
        int clock_error = remaining(condition->clock, deadline, &timeout);
        if (clock_error) {
            error = clock_error;
            break;
        }
    } while (timeout);
    linuxSyncLock();
    lock->owner = self;
    lock->depth = 1;
    --lock->references;
    if (!--condition->waiters) condition->bound = NULL;
    linuxSyncUnlock();
    return error;
}
int linuxPthreadCondWait(LinuxCondition *condition, LinuxMutex *mutex) { return waitCondition(condition, mutex, NULL); }
int linuxPthreadCondTimedWait(LinuxCondition *condition, LinuxMutex *mutex, const LinuxTimespec *deadline) {
    return deadline ? waitCondition(condition, mutex, deadline) : LINUX_EINVAL;
}
static int wakeCondition(LinuxCondition *object, bool all) {
    linuxSyncLock();
    CondRecord *record;
    int error = getCond(object, &record);
    if (!error) error = nativeWake(record->native, all);
    linuxSyncUnlock();
    return error;
}
int linuxPthreadCondSignal(LinuxCondition *condition) { return wakeCondition(condition, false); }
int linuxPthreadCondBroadcast(LinuxCondition *condition) { return wakeCondition(condition, true); }
bool linuxSyncReset(void) {
    linuxSyncLock();
    for (unsigned i = 0; i < LINUX_SYNC_MAX_MUTEXES; ++i)
        if (mutexes[i].owner || mutexes[i].references) {
            linuxSyncUnlock();
            return false;
        }
    for (unsigned i = 0; i < LINUX_SYNC_MAX_CONDITIONS; ++i)
        if (conditions[i].waiters) {
            linuxSyncUnlock();
            return false;
        }
    bool ok = true;
    for (unsigned i = 0; i < LINUX_SYNC_MAX_CONDITIONS; ++i)
        if (conditions[i].object) {
            if (nativeCondDestroy(conditions[i].native))
                ok = false;
            else
                memset(&conditions[i], 0, sizeof(conditions[i]));
        }
    for (unsigned i = 0; i < LINUX_SYNC_MAX_MUTEXES; ++i)
        if (mutexes[i].object) {
            if (nativeMutexDestroy(mutexes[i].native))
                ok = false;
            else
                memset(&mutexes[i], 0, sizeof(mutexes[i]));
        }
    linuxSyncUnlock();
    return ok;
}
