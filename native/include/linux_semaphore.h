#pragma once
#include "linux_abi.h"
#include <stdatomic.h>
#include <stdio.h>
typedef struct {
    _Atomic uint64_t data;
    uint64_t words[3];
} LinuxSemaphore;
#define LINUX_SEM_MAX_OBJECTS 256u
#define LINUX_SEM_VALUE_MAX UINT32_C(2147483647)
#define LINUX_SEM_POLL_NS UINT64_C(100000000)
int linuxSemInit(LinuxSemaphore *, int shared, unsigned value);
int linuxSemDestroy(LinuxSemaphore *);
int linuxSemWait(LinuxSemaphore *);
int linuxSemPost(LinuxSemaphore *);
bool linuxSemReset(void);
