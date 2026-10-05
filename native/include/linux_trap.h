#pragma once
#include "linux_abi.h"
#include <stdio.h>

// Imports without an adapter are not left at zero (a call would jump to address 0): each required function import gets its own tiny
// stub. Weak imports stay null and objects are never replaced.
//
// A stub is NOT an implementation. When called it refuses the call explicitly - errno ENOSYS and the failure value of the function
// (-1, or 0 for functions returning a pointer or a size) - and lets the client go on. The refusals are counted, and logged when the
// diagnostics are on. LINUX_TRAP_REFUSAL_LIMIT refusals in total end the run (a loop on a missing function must not spin forever).
#define LINUX_TRAP_MAX 512u
#define LINUX_TRAP_REFUSAL_LIMIT 20000u
typedef struct {
    unsigned count;
    unsigned requests;
    uint64_t refused;
} LinuxTrapStats;

// The registry first (linuxAbiResolve); only a required function it cannot bind receives a stub.
bool linuxTrapResolve(void *context, const ElfImport *symbol, uintptr_t *address);
const char *linuxTrapName(unsigned index);       // NULL when the index was never assigned
unsigned linuxTrapCalls(unsigned index);         // calls to this import so far (0 for an unassigned index)
uintptr_t linuxTrapFirstCaller(unsigned index);  // link register of its first call, 0 when never called
void linuxTrapReset(void);                       // forget names and counts; callers must be quiescent
LinuxTrapStats linuxTrapStats(void);
// Called by the assembly stubs (x0 = index, x1 = caller's link register); the result is the call's return value.
int64_t linuxTrapReport(unsigned index, uintptr_t caller);
// Start of the stub table: LINUX_TRAP_MAX stubs of LINUX_TRAP_STUB_BYTES bytes each.
#define LINUX_TRAP_STUB_BYTES 8u
extern const unsigned char linuxTrapStubs[];
