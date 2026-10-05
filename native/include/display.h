#pragma once
#include <stdbool.h>

// The console's window is shared by the text console (messages) and the game's GPU surface (Mesa/EGL): one gives it up before the
// other takes it.
// Call only after the previous user has released all of its buffers; false when the window is not free.
bool displayPrepareWindow(void);
// The size of the game's picture: 1280x720 on the console's own screen, 1920x1080 when it is docked (a TV).
void displayGameSize(unsigned *width, unsigned *height);  // for the mode the console is in now
// Gives the window that size. Call after displayPrepareWindow, before the game creates its surface (the SDL layer replaces the surface when
// the size changes during a game).
void displaySetGameResolution(void);
