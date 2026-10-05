#pragma once
#include <stdint.h>

// Keyboard names the way SDL 3 gives them, for a US layout: the game builds its table of key names from these at start-up.
uint32_t linuxSdlKeyFromScancode(uint32_t scancode);  // SDL_GetKeyFromScancode: the keycode, 0 (SDLK_UNKNOWN) for a scancode with none
const char *linuxSdlKeyName(uint32_t keycode);        // SDL_GetKeyName: "A", "Space", "F1", "Left Ctrl"..., "" for an unknown key
