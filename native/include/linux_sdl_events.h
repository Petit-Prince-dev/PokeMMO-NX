#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Input of the virtual SDL3: a queue of SDL_Event records (128 bytes, SDL 3 layout) fed by snapshots of the console's input.
// The touch screen is a mouse (a tap is a left click, a long press a right click), the controller is an SDL gamepad, or a mouse while its cursor is on.
// linux_sdl_input.c reads the console's input.
#define LINUX_SDL_EVENT_BYTES 128u
#define LINUX_SDL_GAMEPAD_ID 1u
#define LINUX_SDL_WINDOW_ID 1u
enum {
    LINUX_SDL_EVENT_QUIT = 0x100,
    LINUX_SDL_EVENT_WINDOW_RESIZED = 0x206,
    LINUX_SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED = 0x207,
    LINUX_SDL_EVENT_KEY_DOWN = 0x300,
    LINUX_SDL_EVENT_KEY_UP = 0x301,
    LINUX_SDL_EVENT_TEXT_INPUT = 0x303,
    LINUX_SDL_EVENT_MOUSE_MOTION = 0x400,
    LINUX_SDL_EVENT_MOUSE_BUTTON_DOWN = 0x401,
    LINUX_SDL_EVENT_MOUSE_BUTTON_UP = 0x402,
    LINUX_SDL_EVENT_GAMEPAD_AXIS_MOTION = 0x650,
    LINUX_SDL_EVENT_GAMEPAD_BUTTON_DOWN = 0x651,
    LINUX_SDL_EVENT_GAMEPAD_BUTTON_UP = 0x652,
    LINUX_SDL_EVENT_GAMEPAD_ADDED = 0x653,
    LINUX_SDL_EVENT_GAMEPAD_REMOVED = 0x654
};
enum { LINUX_SDL_BUTTON_LEFT = 1, LINUX_SDL_BUTTON_RIGHT = 3, LINUX_SDL_BUTTON_LMASK = 1, LINUX_SDL_BUTTON_RMASK = 4 };
enum { LINUX_SDL_GAMEPAD_BUTTONS = 15, LINUX_SDL_GAMEPAD_AXES = 6 };  // SDL_GAMEPAD_BUTTON_SOUTH..DPAD_RIGHT, LEFTX..RIGHT_TRIGGER

typedef struct {
    bool gamepad;      // a controller is connected
    unsigned fingers;  // fingers on the screen
    float x, y;        // first finger, in window pixels
    bool pointer;      // the controller's cursor is on (linux_sdl_cursor.c): a second mouse, which the game sees besides the touch screen
    float pointer_x, pointer_y;
    bool pointer_left, pointer_right;
    uint32_t buttons;  // bit i = SDL_GamepadButton i
    int16_t axes[LINUX_SDL_GAMEPAD_AXES];
    uint64_t timestamp_ns;
} LinuxInputSnapshot;

void linuxSdlEventsReset(void);
void linuxSdlEventsUpdate(const LinuxInputSnapshot *snapshot);  // compares with the previous snapshot and queues the differences
bool linuxSdlEventsPop(void *event);                            // copies the oldest event into a 128-byte buffer
// Keyboard: SDL_EVENT_KEY_DOWN/UP (scancode and keycode as SDL 3 numbers). Backspace, return and tab have helpers below.
enum { LINUX_SDL_SCANCODE_RETURN = 40, LINUX_SDL_SCANCODE_BACKSPACE = 42, LINUX_SDL_KEY_RETURN = 13, LINUX_SDL_KEY_BACKSPACE = 8 };
// The window got a new size (SDL_EVENT_WINDOW_RESIZED and SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED).
void linuxSdlEventsWindowResized(unsigned width, unsigned height, uint64_t timestamp_ns);
void linuxSdlEventsPushKey(uint32_t scancode, uint32_t keycode, bool down, uint64_t timestamp_ns);
// The text of a field changed from `previous` to `current` (what a keyboard that reports whole strings gives): the game is told
// with backspaces for the characters that went away and one text input event for the ones that came. Returns the events queued.
unsigned linuxSdlEventsTextChanged(const char *previous, const char *current, uint64_t timestamp_ns);
unsigned linuxSdlEventsPending(void);
uint32_t linuxSdlEventsMouse(float *x, float *y);  // position and SDL_BUTTON_*MASK bits
int linuxSdlEventsGamepadCount(void);
bool linuxSdlEventsGamepadButton(int button);
int16_t linuxSdlEventsGamepadAxis(int axis);
