#pragma once
#include <stdint.h>
#include <stdio.h>

// Closures for libffi (LWJGL upcalls: SDL log output, event filter, GL debug output ...).
//
// libffi builds a small piece of machine code for every closure and wants memory that is writable and executable.
// Horizon has no such memory: executable code needs a CodeMemory alias that is switched between a writable and an
// executable view. The loader therefore replaces three of libffi's own functions when the client looks them up:
//   ffi_closure_alloc     hands out a slot of a JIT pool (writable address, plus the executable address it will run at)
//   ffi_prep_closure_loc  makes the pool writable, lets libffi write its trampoline, makes the pool executable again
//   ffi_closure_free      does nothing (slots are not reused)
// Everything else of libffi stays untouched. `original` is the address libffi itself exports under `name`.
uintptr_t linuxJitOverride(const char *name, uintptr_t original);  // the replacement, or `original` when `name` is not replaced
void linuxJitReset(void);                                          // releases the pool; no closure may be called afterwards
