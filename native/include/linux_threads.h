#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t LinuxPthread;
typedef uint32_t LinuxPthreadKey;
typedef struct {
    uint64_t words[8];
} LinuxPthreadAttr;
typedef void *(*LinuxThreadEntry)(void *);
typedef struct {
    size_t threads, live_threads, created;
} LinuxThreadStats;
#define LINUX_THREAD_DEFAULT_STACK (1024u * 1024u)
#define LINUX_THREAD_MAX_STACK (16u * 1024u * 1024u)
#define LINUX_THREAD_MAX_KEYS 64u
#define LINUX_THREAD_MAX_THREADS 64u
int linuxPthreadAttrInit(LinuxPthreadAttr *attr);
int linuxPthreadAttrDestroy(LinuxPthreadAttr *attr);
int linuxPthreadAttrSetDetachState(LinuxPthreadAttr *attr, int state);
int linuxPthreadAttrSetStackSize(LinuxPthreadAttr *attr, size_t bytes);
int linuxPthreadAttrGetStack(const LinuxPthreadAttr *attr, void **base, size_t *bytes);
int linuxPthreadAttrGetGuardSize(const LinuxPthreadAttr *attr, size_t *bytes);
int linuxPthreadGetattr(LinuxPthread thread, LinuxPthreadAttr *attr);
int linuxPthreadCreate(LinuxPthread *thread, const LinuxPthreadAttr *attr, LinuxThreadEntry entry, void *argument);
int linuxPthreadJoin(LinuxPthread thread, void **result);
LinuxPthread linuxPthreadSelf(void);
int linuxPthreadDetach(LinuxPthread thread);    // the record is reclaimed after the thread ends (at the next creation or join)
bool linuxPthreadExists(LinuxPthread thread);   // the calling thread or a managed thread not yet joined
bool linuxPthreadManaged(LinuxPthread thread);  // created by linuxPthreadCreate and not yet joined (false for foreign threads)
int linuxPthreadKeyCreate(LinuxPthreadKey *key, void (*destructor)(void *));
int linuxPthreadKeyDelete(LinuxPthreadKey key);
void *linuxPthreadGetSpecific(LinuxPthreadKey key);
int linuxPthreadSetSpecific(LinuxPthreadKey key, const void *value);
int linuxSchedYield(void);
LinuxThreadStats linuxThreadsStats(void);
// Shutdown: join/close every managed thread BEFORE resetting keys
// or unloading callback code. A failed close retains ownership for retry.
bool linuxThreadsReset(void);
// Diagnostics that never take a lock (a watchdog must work when a lock is stuck): a racy read of the thread table.
typedef struct {
    LinuxPthread id;
    uint32_t handle;
    bool started, exited;
} LinuxThreadSnapshot;
unsigned linuxThreadsLiveUnlocked(void);
unsigned linuxThreadsSnapshot(LinuxThreadSnapshot *out, unsigned max);
// A core that is not the one of the game's main thread (for the threads of this project that work in the background).
int linuxThreadBackgroundCore(void);
void linuxThreadYield(void);  // gives the core to another thread (a spin loop waiting for a lock must call it)
