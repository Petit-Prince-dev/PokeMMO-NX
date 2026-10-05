#include "linux_sdl_input.h"
#include "diagnostics.h"
#include "linux_threads.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

static void trace(const char *format, ...) {
    static atomic_uint lines;
    if (atomic_fetch_add(&lines, 1) > 400) return;
    va_list args;
    va_start(args, format);
    diagnosticsTraceV(format, args);
    va_end(args);
}

typedef enum { KB_IDLE = 0, KB_LAUNCHING, KB_READY, KB_APPEARING, KB_VISIBLE, KB_DISAPPEARING, KB_CLOSING } KeyboardPhase;
static SwkbdInline keyboard;
static volatile KeyboardPhase kb_phase;
static PadState pad;
static bool pad_ready;
static atomic_flag busy = ATOMIC_FLAG_INIT;
static void lockInput(void) {
    while (atomic_flag_test_and_set_explicit(&busy, memory_order_acquire)) {}
}
static void unlockInput(void) { atomic_flag_clear_explicit(&busy, memory_order_release); }

// SDL names the face buttons by position (south is the lower one), the game by letter (its A confirms, its B cancels) and the
// player reads the letters printed on the console: the letter wins, so A, B, X and Y trade places with their SDL positions.
static uint32_t buttonBits(u64 held) {
    static const struct {
        u64 hid;
        int sdl;
    } map[] = {{HidNpadButton_A, 0},    {HidNpadButton_B, 1},      {HidNpadButton_X, 2},      {HidNpadButton_Y, 3},     {HidNpadButton_Minus, 4},
               {HidNpadButton_Plus, 6}, {HidNpadButton_StickL, 7}, {HidNpadButton_StickR, 8}, {HidNpadButton_L, 9},     {HidNpadButton_R, 10},
               {HidNpadButton_Up, 11},  {HidNpadButton_Down, 12},  {HidNpadButton_Left, 13},  {HidNpadButton_Right, 14}};
    uint32_t bits = 0;
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
        if (held & map[i].hid) bits |= 1u << map[i].sdl;
    return bits;
}
static int16_t clampAxis(int value) { return value > 32767 ? 32767 : (value < -32767 ? -32767 : (int16_t)value); }

bool linuxSdlInputSample(LinuxInputSnapshot *snapshot) {
    lockInput();
    if (!pad_ready) {
        padConfigureInput(8, HidNpadStyleSet_NpadStandard);
        padInitializeAny(&pad);
        hidInitializeTouchScreen();
        pad_ready = true;
    }
    padUpdate(&pad);
    u64 held = padGetButtons(&pad);
    *snapshot = (LinuxInputSnapshot){0};
    snapshot->timestamp_ns = armTicksToNs(armGetSystemTick());
    snapshot->gamepad = padIsConnected(&pad);
    snapshot->buttons = buttonBits(held);
    HidAnalogStickState left = padGetStickPos(&pad, 0), right = padGetStickPos(&pad, 1);
    snapshot->axes[0] = clampAxis(left.x);
    snapshot->axes[1] = clampAxis(-left.y);  // libnx has up positive, SDL has up negative
    snapshot->axes[2] = clampAxis(right.x);
    snapshot->axes[3] = clampAxis(-right.y);
    snapshot->axes[4] = (held & HidNpadButton_ZL) ? 32767 : 0;
    snapshot->axes[5] = (held & HidNpadButton_ZR) ? 32767 : 0;
    HidTouchScreenState touch = {0};
    if (hidGetTouchScreenStates(&touch, 1) && touch.count > 0) {
        snapshot->fingers = (unsigned)touch.count;
        snapshot->x = (float)touch.touches[0].x;
        snapshot->y = (float)touch.touches[0].y;
        // The keyboard is on top of the game: touches on its keys must not click what is under it.
        if (kb_phase >= KB_APPEARING && kb_phase <= KB_DISAPPEARING && swkbdInlineIsUsedTouchPointByKeyboard(&keyboard, (s32)snapshot->x, (s32)snapshot->y))
            snapshot->fingers = 0;
    }
    unlockInput();
    return true;
}

// ---- inline keyboard ---------------------------------------------------------------------------------------------------------
static atomic_bool kb_want, kb_failed, kb_initialized, kb_busy;
static char kb_text[512];
#define KB_PREFIX 64
static uint64_t kb_launch_ns;
static uint64_t nowNs(void) { return armTicksToNs(armGetSystemTick()); }
static void onInitialized(void) {
    atomic_store(&kb_initialized, true);
    trace("swkbd.inline=INITIALIZED");
}
// The log gets the length and the kinds of characters, never the text itself (it may be a password).
static void describe(const char *text, unsigned *letters, unsigned *digits, unsigned *others) {
    *letters = *digits = *others = 0;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))
            ++*letters;
        else if (*p >= '0' && *p <= '9')
            ++*digits;
        else
            ++*others;
    }
}
static void onChanged(const char *text, SwkbdChangedStringArg *argument) {
    unsigned queued = linuxSdlEventsTextChanged(kb_text, text, nowNs());
    snprintf(kb_text, sizeof(kb_text), "%s", text ? text : "");
    unsigned letters, digits, others;
    describe(kb_text, &letters, &digits, &others);
    trace("swkbd.inline=CHANGED length=%zu letters=%u digits=%u others=%u applet_length=%u cursor=%d events=%u", strlen(kb_text), letters, digits, others,
          argument ? argument->stringLen : 0u, argument ? argument->cursorPos : -1, queued);
}
static void onEnter(const char *text, SwkbdDecidedEnterArg *argument) {
    (void)argument;
    unsigned queued = linuxSdlEventsTextChanged(kb_text, text, nowNs());
    snprintf(kb_text, sizeof(kb_text), "%s", text ? text : "");
    linuxSdlEventsPushKey(LINUX_SDL_SCANCODE_RETURN, LINUX_SDL_KEY_RETURN, true, nowNs());
    linuxSdlEventsPushKey(LINUX_SDL_SCANCODE_RETURN, LINUX_SDL_KEY_RETURN, false, nowNs());
    atomic_store(&kb_want, false);
    trace("swkbd.inline=ENTER length=%zu events=%u", strlen(kb_text), queued);
}
static void onCancel(void) {
    atomic_store(&kb_want, false);
    trace("swkbd.inline=CANCEL");
}
static void closeKeyboard(void) {
    swkbdInlineClose(&keyboard);
    kb_phase = KB_IDLE;
    atomic_store(&kb_initialized, false);
}
// The applet needs about 0.3 s to exit: a helper thread waits for it so the game does not stall. The next keyboard waits for IDLE.
static Thread kb_closer;
static bool kb_closer_started;
static void closerMain(void *unused) {
    (void)unused;
    uint64_t start = nowNs();
    closeKeyboard();
    trace("swkbd.inline=CLOSED_ASYNC ms=%u", (unsigned)((nowNs() - start) / 1000000ull));
}
static void closeKeyboardLater(void) {
    kb_phase = KB_CLOSING;
    if (R_SUCCEEDED(threadCreate(&kb_closer, closerMain, NULL, NULL, 0x8000, 0x3b, linuxThreadBackgroundCore())) && R_SUCCEEDED(threadStart(&kb_closer)))
        kb_closer_started = true;
    else {
        trace("swkbd.inline=CLOSER_THREAD_FAILED");
        closeKeyboard();
    }
}
void linuxSdlInputKeyboardRequest(bool show) { atomic_store(&kb_want, show); }
bool linuxSdlInputKeyboardVisible(void) { return kb_phase >= KB_APPEARING && kb_phase <= KB_VISIBLE; }
bool linuxSdlInputKeyboardAvailable(void) { return !atomic_load(&kb_failed); }
void linuxSdlInputKeyboardPump(void) {
    if (atomic_exchange(&kb_busy, true)) return;  // one caller at a time; the others come back next frame
    bool want = atomic_load(&kb_want);
    if (kb_phase == KB_CLOSING) {
        atomic_store(&kb_busy, false);
        return;
    }
    if (kb_phase == KB_IDLE && kb_closer_started) {
        threadWaitForExit(&kb_closer);
        threadClose(&kb_closer);
        kb_closer_started = false;
    }
    if (kb_phase == KB_IDLE && want && !atomic_load(&kb_failed)) {
        Result rc = swkbdInlineCreate(&keyboard);
        if (R_SUCCEEDED(rc)) rc = swkbdInlineLaunchForLibraryApplet(&keyboard, SwkbdInlineMode_AppletDisplay, 0);
        trace("swkbd.inline=LAUNCH rc=0x%x", rc);
        if (R_FAILED(rc)) {
            swkbdInlineClose(&keyboard);
            atomic_store(&kb_failed, true);
        } else {
            swkbdInlineSetFinishedInitializeCallback(&keyboard, onInitialized);
            swkbdInlineSetChangedStringCallback(&keyboard, onChanged);
            swkbdInlineSetDecidedEnterCallback(&keyboard, onEnter);
            swkbdInlineSetDecidedCancelCallback(&keyboard, onCancel);
            atomic_store(&kb_initialized, false);
            kb_launch_ns = nowNs();
            kb_phase = KB_LAUNCHING;
        }
    }
    if (kb_phase != KB_IDLE) {
        SwkbdState state = SwkbdState_Inactive;
        Result rc = swkbdInlineUpdate(&keyboard, &state);
        if (R_FAILED(rc) || (state == SwkbdState_Inactive && kb_phase > KB_LAUNCHING)) {
            trace("swkbd.inline=ENDED rc=0x%x state=%d", rc, (int)state);
            closeKeyboard();
            if (R_FAILED(rc)) atomic_store(&kb_failed, true);
        } else if (kb_phase == KB_LAUNCHING && atomic_load(&kb_initialized))
            kb_phase = KB_READY;
        else if (kb_phase == KB_LAUNCHING && nowNs() - kb_launch_ns > 8000000000ull) {
            trace("swkbd.inline=LAUNCH_TIMEOUT");
            closeKeyboard();
            atomic_store(&kb_failed, true);
        }
        if (kb_phase == KB_READY && want) {
            SwkbdAppearArg arg;
            swkbdInlineMakeAppearArg(&arg, SwkbdType_QWERTY);  // letters and the number row on the first page
            swkbdInlineAppearArgSetStringLenMax(&arg, 128 + KB_PREFIX);
            arg.keySetDisableBitmask = 0;  // every key stays available
            arg.dicFlag = 0;               // no word suggestions: they only get in the way of a login
            trace("swkbd.inline=APPEAR_ARG type=%d keyset_disabled=0x%x length_max=%d length_min=%d flags=0x%x", (int)arg.type, arg.keySetDisableBitmask,
                  arg.stringLenMax, arg.stringLenMin, arg.flags);
            // The game does not tell what its field holds. The keyboard starts with KB_PREFIX spaces (invisible, the cursor after them): the
            // backspaces of the player erase them one by one, and each one reaches the game as a backspace, so text already in the field can go.
            char prefix[KB_PREFIX + 1];
            memset(prefix, ' ', KB_PREFIX);
            prefix[KB_PREFIX] = 0;
            snprintf(kb_text, sizeof(kb_text), "%s", prefix);  // so that setting it is not mistaken for typing
            swkbdInlineSetInputText(&keyboard, prefix);
            swkbdInlineSetCursorPos(&keyboard, KB_PREFIX);
            swkbdInlineAppear(&keyboard, &arg);
            trace("swkbd.inline=APPEAR");
            kb_phase = KB_APPEARING;
        } else if (kb_phase == KB_APPEARING && state == SwkbdState_Shown) {
            kb_phase = KB_VISIBLE;
            trace("swkbd.inline=SHOWN");
        } else if ((kb_phase == KB_VISIBLE || kb_phase == KB_APPEARING) && !want) {
            swkbdInlineDisappear(&keyboard);
            kb_phase = KB_DISAPPEARING;
            trace("swkbd.inline=DISAPPEAR");
        }
        // Once hidden (by us, by Enter or by cancel) the applet is closed: the next appearance starts a new one, like the first, which typed fine.
        else if (kb_phase == KB_DISAPPEARING && state == SwkbdState_Initialized) {
            trace("swkbd.inline=DISAPPEARED");
            closeKeyboardLater();
        } else if (kb_phase == KB_VISIBLE && state == SwkbdState_Initialized) {
            atomic_store(&kb_want, false);
            trace("swkbd.inline=HIDDEN_BY_APPLET");
            closeKeyboardLater();
        }
    }
    atomic_store(&kb_busy, false);
}
