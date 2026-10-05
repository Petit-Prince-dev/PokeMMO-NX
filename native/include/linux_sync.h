#pragma once
#include "linux_abi.h"
#include <stdint.h>
#include <stdio.h>
typedef struct {
    uint64_t words[6];
} LinuxMutex;
typedef struct {
    uint64_t words[6];
} LinuxCondition;
typedef struct {
    uint32_t words[2];
} LinuxMutexAttr;
typedef struct {
    uint32_t words[2];
} LinuxConditionAttr;
#define LINUX_SYNC_MAX_MUTEXES 1024u
#define LINUX_SYNC_MAX_CONDITIONS 1024u
int linuxPthreadMutexInit(LinuxMutex *, const LinuxMutexAttr *);
int linuxPthreadMutexDestroy(LinuxMutex *);
int linuxPthreadMutexLock(LinuxMutex *);
int linuxPthreadMutexTryLock(LinuxMutex *);
int linuxPthreadMutexUnlock(LinuxMutex *);
int linuxPthreadCondAttrInit(LinuxConditionAttr *);
int linuxPthreadCondAttrDestroy(LinuxConditionAttr *);
int linuxPthreadCondAttrSetClock(LinuxConditionAttr *, int);
int linuxPthreadCondInit(LinuxCondition *, const LinuxConditionAttr *);
int linuxPthreadCondDestroy(LinuxCondition *);
int linuxPthreadCondWait(LinuxCondition *, LinuxMutex *);
int linuxPthreadCondTimedWait(LinuxCondition *, LinuxMutex *, const LinuxTimespec *);
int linuxPthreadCondSignal(LinuxCondition *);
int linuxPthreadCondBroadcast(LinuxCondition *);
// Cleanup AFTER joining workers. Never write to caller addresses:
// they may refer to expired stack frames. Busy/failed objects stay owned.
bool linuxSyncReset(void);
// The lock of the registries of the adapters (mutexes, conditions, semaphores, descriptors). Short critical sections only.
void linuxSyncLock(void);
void linuxSyncUnlock(void);
