#pragma once
#include "linux_dl.h"
#include <stdbool.h>
#include <stdio.h>

// This project's own implementations of the shared libraries the client's Java layer loads by name (LWJGL binds them with
// dlsym). They are "virtual": no ELF file exists for them, the functions below are the library.
//   libSDL3   window, OpenGL context (EGL on the Switch's window), events, time, gamepads (subset)
//   libGLX    the same entry points under the name LWJGL tries first
//   libEGL    eglGetProcAddress and the GL entry points of Mesa's EGL
//   libopenal OpenAL 1.1 on a software mixer (linux_audio.c) feeding the console's audio output
extern const LinuxVirtualLibrary linuxSdlLibrary, linuxEglLibrary, linuxGlxLibrary, linuxOpenAlLibrary;

void linuxSdlStopRequest(void);  // the next SDL_PollEvent reports SDL_EVENT_QUIT (applet exit)
// The picture should get this size (the console was docked or undocked). Any thread may ask; the thread that renders does it between two frames.
void linuxSdlRequestSize(unsigned width, unsigned height);
bool linuxSdlReset(void);  // tears down the EGL surface and context created for the window
// For the file chooser, when the game calls it on the thread that renders: whether this thread owns the GL context, and one frame of the
// chooser (input sampled, picture swapped) so that it can run its own loop.
bool linuxSdlThreadHasContext(void);
bool linuxSdlPresentFrame(void);

typedef struct {
    unsigned windows, contexts, frames, calls, unknown_lookups;
    bool gl_ready;
    unsigned width, height;
} LinuxSdlStats;
LinuxSdlStats linuxSdlStats(void);
