#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// SDL_IOStream over a block of memory (SDL_IOFromMem / SDL_IOFromConstMem): the game reads its gamepad database through one.
// A stream is the pointer this returns; eight can be open at a time. Seek positions are clamped to the block like SDL does.
void *linuxSdlIoFromMemory(void *memory, size_t size, bool writable);
size_t linuxSdlIoRead(void *stream, void *buffer, size_t size);         // bytes read, 0 at the end or for a stream that is not open
size_t linuxSdlIoWrite(void *stream, const void *buffer, size_t size);  // bytes written, 0 for read-only memory
int64_t linuxSdlIoSeek(void *stream, int64_t offset, int whence);       // whence: 0 start, 1 current, 2 end; the new position or -1
int64_t linuxSdlIoTell(void *stream);
int64_t linuxSdlIoSize(void *stream);  // -1 for a stream that is not open
bool linuxSdlIoClose(void *stream);
// Reads the rest of a gamepad database and returns how many mapping lines it holds (a line with a comma that is not a comment), -1 if the
// stream is not open. The mappings themselves are not used: the console's controller is described by this project.
int linuxSdlIoCountMappings(void *stream);
void linuxSdlIoReset(void);
