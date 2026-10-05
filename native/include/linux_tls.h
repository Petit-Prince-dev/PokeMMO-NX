#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Static thread-local storage for shared libraries loaded at run time (AArch64, TLS descriptors).
// Every managed thread owns a small arena; the thread pointer register (TPIDR_EL0) points at its start and a
// TLS variable lives at thread_pointer + offset. A module registered after threads started is initialized in the
// arenas of all attached threads, so the offsets handed to the relocation code stay valid for every thread.
#define LINUX_TLS_ARENA_BYTES 16384u
#define LINUX_TLS_FIRST_OFFSET 64u  // room for a thread control block before the first module
#define LINUX_TLS_MAX_MODULES 8u
#define LINUX_TLS_MAX_THREADS 256u

// Reserves space for one module and copies its initialization image (file_bytes) into every attached arena;
// the remainder up to memory_bytes is zero. False when the arena or module table is full.
bool linuxTlsRegisterModule(const void *image, size_t file_bytes, size_t memory_bytes, size_t alignment, uint64_t *offset);
// Called by the thread layer on the new thread, before and after its entry runs. Attach allocates the arena and
// sets the thread pointer; detach clears the thread pointer and frees the arena.
bool linuxTlsAttachThread(void);
void linuxTlsDetachThread(void);
// Forget every module (quiescent callers only: no thread may be attached).
bool linuxTlsReset(void);
unsigned linuxTlsModuleCount(void);
