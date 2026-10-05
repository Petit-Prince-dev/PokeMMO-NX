#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Crash recorder. On Horizon an unhandled fault (bad pointer, jump to 0, bad instruction) would end the
// application with no explanation. The recorder (libnx's exception handler, always installed) writes the registers, the fault
// address, a stack excerpt and a frame-pointer backtrace to the diagnostics log when there is one, then ends the process.
typedef struct {
    uint32_t description, pstate, esr, afsr0, afsr1;
    uint64_t x[29], fp, lr, sp, pc, far;
} CrashContext;
typedef struct {
    const char *name;
    uintptr_t base;
    uint64_t span;
} CrashRegion;  // a named code range, e.g. the client
// Reads memory without faulting; false when the address is unreadable.
typedef bool (*CrashReader)(uintptr_t address, void *out, size_t bytes);

#define CRASH_MAX_FRAMES 32u
#define CRASH_STACK_WORDS 48u

// Portable and allocation-free. `read` may be NULL (registers only).
void crashFormat(FILE *out, const CrashContext *context, const CrashRegion *regions, unsigned region_count, CrashReader read);

// crash_recorder.c. The regions must stay valid for the life of the process.
void crashRecorderSetRegions(const CrashRegion *regions, unsigned count);
