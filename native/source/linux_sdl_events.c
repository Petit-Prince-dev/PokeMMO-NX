#include "linux_sdl_events.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE 256u
#define TEXT_SLOTS 16u
#define AXIS_STEP 256               // smaller changes of a stick are not worth an event
#define LONG_PRESS_NS 500000000ull  // a finger that stays where it landed this long is a right click
#define CLICK_NS 50000000ull        // a click lasts this long, so that a game that reads the button state (not the events) sees it
#define TAP_SLOP 20.0f              // window pixels a finger may move and still be a tap or a long press; further it drags

static unsigned char queue[QUEUE][LINUX_SDL_EVENT_BYTES];
static unsigned head, tail, count;
static LinuxInputSnapshot last_snapshot;
static bool have_previous;
static float mouse_x, mouse_y;
static uint32_t mouse_buttons;
// A finger on the screen is undecided (waiting: a tap or a long press), a drag (left button held) or spent (the long press was done).
enum { TOUCH_NONE, TOUCH_WAITING, TOUCH_DRAGGING, TOUCH_SPENT };
static int touch_state;
static uint64_t touch_start_ns, release_ns;
static float touch_x, touch_y;
static int release_button;  // the button whose release is due at release_ns, 0 for none
static char text_slots[TEXT_SLOTS][256];
static unsigned text_next;
static atomic_flag lock = ATOMIC_FLAG_INIT;
static void lockQueue(void) {
    while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire)) {}
}
static void unlockQueue(void) { atomic_flag_clear_explicit(&lock, memory_order_release); }

static void put32(unsigned char *event, size_t offset, uint32_t value) { memcpy(event + offset, &value, sizeof(value)); }
static void putFloat(unsigned char *event, size_t offset, float value) { memcpy(event + offset, &value, sizeof(value)); }
static void header(unsigned char *event, uint32_t type, uint64_t timestamp) {
    memset(event, 0, LINUX_SDL_EVENT_BYTES);
    put32(event, 0, type);
    memcpy(event + 8, &timestamp, sizeof(timestamp));
}
static void push(const unsigned char *event) {
    // Called with the lock held. A full queue drops the oldest event: input must never block the game.
    if (count == QUEUE) {
        head = (head + 1) % QUEUE;
        --count;
    }
    memcpy(queue[tail], event, LINUX_SDL_EVENT_BYTES);
    tail = (tail + 1) % QUEUE;
    ++count;
}

void linuxSdlEventsReset(void) {
    lockQueue();
    head = tail = count = 0;
    memset(&last_snapshot, 0, sizeof(last_snapshot));
    have_previous = false;
    mouse_x = mouse_y = 0;
    mouse_buttons = 0;
    touch_state = TOUCH_NONE;
    release_button = 0;
    text_next = 0;
    memset(text_slots, 0, sizeof(text_slots));
    unlockQueue();
}

static void mouseMotion(uint64_t time, float x, float y, float dx, float dy) {
    unsigned char event[LINUX_SDL_EVENT_BYTES];
    header(event, LINUX_SDL_EVENT_MOUSE_MOTION, time);
    put32(event, 16, LINUX_SDL_WINDOW_ID);
    put32(event, 20, 1);
    put32(event, 24, mouse_buttons);
    putFloat(event, 28, x);
    putFloat(event, 32, y);
    putFloat(event, 36, dx);
    putFloat(event, 40, dy);
    push(event);
}
static void mouseButton(uint64_t time, bool down, int button, float x, float y) {
    unsigned char event[LINUX_SDL_EVENT_BYTES];
    header(event, down ? LINUX_SDL_EVENT_MOUSE_BUTTON_DOWN : LINUX_SDL_EVENT_MOUSE_BUTTON_UP, time);
    put32(event, 16, LINUX_SDL_WINDOW_ID);
    put32(event, 20, 1);
    event[24] = (unsigned char)button;
    event[25] = down;
    event[26] = 1;
    putFloat(event, 28, x);
    putFloat(event, 32, y);
    push(event);
}
static void buttonDown(uint64_t time, int button) {
    mouse_buttons |= button == LINUX_SDL_BUTTON_LEFT ? LINUX_SDL_BUTTON_LMASK : LINUX_SDL_BUTTON_RMASK;
    mouseButton(time, true, button, mouse_x, mouse_y);
}
static void buttonUp(uint64_t time, int button) {
    mouse_buttons &= ~(uint32_t)(button == LINUX_SDL_BUTTON_LEFT ? LINUX_SDL_BUTTON_LMASK : LINUX_SDL_BUTTON_RMASK);
    mouseButton(time, false, button, mouse_x, mouse_y);
}
// The release of a click that is due (or at once with `force`).
static void releaseClick(uint64_t time, bool force) {
    if (!release_button || (!force && time < release_ns)) return;
    buttonUp(time, release_button);
    release_button = 0;
}
static void click(uint64_t time, int button) {
    releaseClick(time, true);
    buttonDown(time, button);
    release_button = button;
    release_ns = time + CLICK_NS;
}
static void gamepadDevice(uint64_t time, uint32_t type) {
    unsigned char event[LINUX_SDL_EVENT_BYTES];
    header(event, type, time);
    put32(event, 16, LINUX_SDL_GAMEPAD_ID);
    push(event);
}
static void gamepadButton(uint64_t time, int button, bool down) {
    unsigned char event[LINUX_SDL_EVENT_BYTES];
    header(event, down ? LINUX_SDL_EVENT_GAMEPAD_BUTTON_DOWN : LINUX_SDL_EVENT_GAMEPAD_BUTTON_UP, time);
    put32(event, 16, LINUX_SDL_GAMEPAD_ID);
    event[20] = (unsigned char)button;
    event[21] = down;
    push(event);
}
static void gamepadAxis(uint64_t time, int axis, int16_t value) {
    unsigned char event[LINUX_SDL_EVENT_BYTES];
    header(event, LINUX_SDL_EVENT_GAMEPAD_AXIS_MOTION, time);
    put32(event, 16, LINUX_SDL_GAMEPAD_ID);
    event[20] = (unsigned char)axis;
    memcpy(event + 24, &value, sizeof(value));
    push(event);
}

void linuxSdlEventsWindowResized(unsigned width, unsigned height, uint64_t timestamp_ns) {
    lockQueue();
    for (uint32_t type = LINUX_SDL_EVENT_WINDOW_RESIZED; type <= LINUX_SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED; ++type) {
        unsigned char event[LINUX_SDL_EVENT_BYTES];
        header(event, type, timestamp_ns);
        put32(event, 16, LINUX_SDL_WINDOW_ID);
        put32(event, 20, width);
        put32(event, 24, height);
        push(event);
    }
    unlockQueue();
}

void linuxSdlEventsUpdate(const LinuxInputSnapshot *now) {
    lockQueue();
    LinuxInputSnapshot before = have_previous ? last_snapshot : (LinuxInputSnapshot){0};
    uint64_t time = now->timestamp_ns;
    // touch screen as a mouse (the first finger): a tap is a left click made when the finger lifts, a finger that stays where it landed is a
    // right click, a finger that moves drags with the left button held from where it landed
    releaseClick(time, false);
    bool touching_before = before.fingers > 0, touching_now = now->fingers > 0;
    if (touching_now) {
        if (!touching_before) {
            releaseClick(time, true);
            mouse_x = touch_x = now->x;
            mouse_y = touch_y = now->y;
            mouseMotion(time, mouse_x, mouse_y, 0, 0);
            touch_state = TOUCH_WAITING;
            touch_start_ns = time;
        } else if (touch_state != TOUCH_SPENT && (now->x != mouse_x || now->y != mouse_y)) {
            float from_x = now->x - touch_x, from_y = now->y - touch_y;
            if (touch_state == TOUCH_WAITING && from_x * from_x + from_y * from_y > TAP_SLOP * TAP_SLOP) {
                buttonDown(time, LINUX_SDL_BUTTON_LEFT);
                touch_state = TOUCH_DRAGGING;
            }
            float dx = now->x - mouse_x, dy = now->y - mouse_y;
            mouse_x = now->x;
            mouse_y = now->y;
            mouseMotion(time, mouse_x, mouse_y, dx, dy);
        }
        if (touch_state == TOUCH_WAITING && time - touch_start_ns >= LONG_PRESS_NS) {
            click(time, LINUX_SDL_BUTTON_RIGHT);
            touch_state = TOUCH_SPENT;
        }
    } else if (touching_before) {
        if (touch_state == TOUCH_WAITING) click(time, LINUX_SDL_BUTTON_LEFT);
        if (touch_state == TOUCH_DRAGGING) buttonUp(time, LINUX_SDL_BUTTON_LEFT);
        touch_state = TOUCH_NONE;
    }
    // controller
    if (now->gamepad && !before.gamepad) gamepadDevice(time, LINUX_SDL_EVENT_GAMEPAD_ADDED);
    if (!now->gamepad && before.gamepad) gamepadDevice(time, LINUX_SDL_EVENT_GAMEPAD_REMOVED);
    if (now->gamepad) {
        uint32_t changed = now->buttons ^ before.buttons;
        for (int button = 0; button < LINUX_SDL_GAMEPAD_BUTTONS; ++button)
            if (changed & (1u << button)) gamepadButton(time, button, (now->buttons >> button) & 1u);
        for (int axis = 0; axis < LINUX_SDL_GAMEPAD_AXES; ++axis) {
            int difference = now->axes[axis] - before.axes[axis];
            bool to_rest = now->axes[axis] == 0 && before.axes[axis] != 0;
            if (difference >= AXIS_STEP || difference <= -AXIS_STEP || to_rest)
                gamepadAxis(time, axis, now->axes[axis]);
            else if (!have_previous && now->axes[axis])
                gamepadAxis(time, axis, now->axes[axis]);
        }
    }
    last_snapshot = *now;
    // keep the reported axes exact even when an event was skipped (small steps): the state is read directly
    have_previous = true;
    unlockQueue();
}

bool linuxSdlEventsPop(void *event) {
    lockQueue();
    bool have = count > 0;
    if (have) {
        if (event) memcpy(event, queue[head], LINUX_SDL_EVENT_BYTES);
        head = (head + 1) % QUEUE;
        --count;
    }
    unlockQueue();
    return have;
}
// One SDL_EVENT_TEXT_INPUT; the text is kept until the queue is reset.
static bool pushText(const char *utf8, uint64_t timestamp_ns) {
    if (!utf8 || !*utf8) return false;
    lockQueue();
    char *slot = text_slots[text_next++ % TEXT_SLOTS];
    snprintf(slot, sizeof(text_slots[0]), "%s", utf8);
    unsigned char event[LINUX_SDL_EVENT_BYTES];
    header(event, LINUX_SDL_EVENT_TEXT_INPUT, timestamp_ns);
    put32(event, 16, LINUX_SDL_WINDOW_ID);
    const char *pointer = slot;
    memcpy(event + 24, &pointer, sizeof(pointer));  // text pointer at offset 24 (after the 4-byte window id and 4 bytes of padding)
    push(event);
    unlockQueue();
    return true;
}
void linuxSdlEventsPushKey(uint32_t scancode, uint32_t keycode, bool down, uint64_t timestamp_ns) {
    unsigned char event[LINUX_SDL_EVENT_BYTES];
    header(event, down ? LINUX_SDL_EVENT_KEY_DOWN : LINUX_SDL_EVENT_KEY_UP, timestamp_ns);
    put32(event, 16, LINUX_SDL_WINDOW_ID);
    put32(event, 20, 1);
    put32(event, 24, scancode);
    put32(event, 28, keycode);
    event[36] = down;
    lockQueue();
    push(event);
    unlockQueue();
}
static bool utf8Continuation(unsigned char byte) { return (byte & 0xc0) == 0x80; }
unsigned linuxSdlEventsTextChanged(const char *previous, const char *current, uint64_t timestamp_ns) {
    if (!previous) previous = "";
    if (!current) current = "";
    size_t common = 0;
    while (previous[common] && previous[common] == current[common]) ++common;
    while (common && (utf8Continuation((unsigned char)previous[common]) || utf8Continuation((unsigned char)current[common])))
        --common;  // back to a character boundary
    unsigned queued = 0;
    for (const char *p = previous + common; *p; ++p)
        if (!utf8Continuation((unsigned char)*p)) {
            linuxSdlEventsPushKey(LINUX_SDL_SCANCODE_BACKSPACE, LINUX_SDL_KEY_BACKSPACE, true, timestamp_ns);
            linuxSdlEventsPushKey(LINUX_SDL_SCANCODE_BACKSPACE, LINUX_SDL_KEY_BACKSPACE, false, timestamp_ns);
            queued += 2;
        }
    if (current[common] && pushText(current + common, timestamp_ns)) ++queued;
    return queued;
}
unsigned linuxSdlEventsPending(void) {
    lockQueue();
    unsigned value = count;
    unlockQueue();
    return value;
}
uint32_t linuxSdlEventsMouse(float *x, float *y) {
    lockQueue();
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    uint32_t value = mouse_buttons;
    unlockQueue();
    return value;
}
int linuxSdlEventsGamepadCount(void) {
    lockQueue();
    int value = have_previous && last_snapshot.gamepad ? 1 : 0;
    unlockQueue();
    return value;
}
bool linuxSdlEventsGamepadButton(int button) {
    lockQueue();
    bool value = have_previous && last_snapshot.gamepad && button >= 0 && button < LINUX_SDL_GAMEPAD_BUTTONS && ((last_snapshot.buttons >> button) & 1u);
    unlockQueue();
    return value;
}
int16_t linuxSdlEventsGamepadAxis(int axis) {
    lockQueue();
    int16_t value = have_previous && last_snapshot.gamepad && axis >= 0 && axis < LINUX_SDL_GAMEPAD_AXES ? last_snapshot.axes[axis] : 0;
    unlockQueue();
    return value;
}
