#pragma once
#include <stdint.h>
#include <stdio.h>

// Functions a virtual library (SDL3, OpenAL) is asked for but does not implement still need an address: LWJGL
// resolves every function of a class when the class loads and fails if one is missing, even if the game never calls it.
// Each such name gets its own tiny function that returns 0 (NULL / false / no id) and logs its first call, so the
// log lists exactly which of them the game really uses.
uintptr_t linuxVirtualStub(const char *name);  // 0 when the pool is exhausted
void linuxVirtualStubsReset(void);
// Called by the stubs themselves (assembly): remembers the first call of each. The result is the stub's return value for its caller.
uintptr_t linuxVirtualStubCalled(unsigned slot);
