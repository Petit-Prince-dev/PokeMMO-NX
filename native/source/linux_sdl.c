#include "linux_sdl.h"
#include "diagnostics.h"
#include "linux_abi.h"
#include "display.h"
#include "linux_audio.h"
#include "linux_file_picker.h"
#include "linux_format.h"
#include "linux_threads.h"
#include "linux_sdl_cursor.h"
#include "linux_sdl_events.h"
#include "linux_sdl_io.h"
#include "linux_sdl_keys.h"
#include "linux_sdl_input.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

static atomic_uint call_counter, unknown_counter, frame_counter;
static atomic_bool stop_requested;
static unsigned logged_unknown;
static void trace(const char *format, ...) {
    static atomic_uint lines;
    if (atomic_fetch_add(&lines, 1) > 600) return;
    va_list args;
    va_start(args, format);
    diagnosticsTraceV(format, args);
    va_end(args);
}
void linuxSdlStopRequest(void) { atomic_store(&stop_requested, true); }

// ---- tiny helpers for the many functions that only need a fixed answer -------------------------------------------------
static bool sdlTrue(void) { return true; }
static bool sdlFalse(void) { return false; }
static uintptr_t sdlNull(void) { return 0; }
static void sdlVoid(void) {}
static float sdlOneFloat(void) { return 1.0f; }

// ---- init, errors, hints, version -----------------------------------------------------------------------------------------
static char error_text[256];
static bool sdlInit(uint32_t flags) {
    trace("sdl.Init flags=0x%x", flags);
    return true;
}
static bool sdlInitSubSystem(uint32_t flags) {
    trace("sdl.InitSubSystem flags=0x%x", flags);
    return true;
}
static uint32_t sdlWasInit(uint32_t flags) { return flags; }
static const char *sdlGetError(void) { return error_text; }
static bool sdlClearError(void) {
    error_text[0] = 0;
    return true;
}
static bool sdlSetError(const char *format, ...) {
    va_list args;
    va_start(args, format);
    linuxFormatVsnprintf(error_text, sizeof(error_text), format, args);
    va_end(args);
    trace("sdl.SetError %s", error_text);
    return false;
}
static bool sdlSetHint(const char *name, const char *value) {
    trace("sdl.SetHint %s=%s", name ? name : "?", value ? value : "?");
    return true;
}
static bool sdlSetHintWithPriority(const char *name, const char *value, int priority) {
    (void)priority;
    return sdlSetHint(name, value);
}
static int sdlGetVersion(void) { return 3004000; }
static const char *sdlGetRevision(void) { return "switch-virtual"; }
static const char *sdlGetPlatform(void) { return "Nintendo Switch"; }
static void *sdlMalloc(size_t bytes) { return linuxAbiMalloc(bytes); }
static void *sdlCalloc(size_t count, size_t bytes) { return linuxAbiCalloc(count, bytes); }
static void *sdlRealloc(void *memory, size_t bytes) { return linuxAbiRealloc(memory, bytes); }
static void sdlFree(void *memory) { linuxAbiFree(memory); }

// ---- properties (name -> number/pointer/boolean/string, identified by small integers) -------------------------------
#define MAX_PROPERTY_SETS 16u
#define MAX_PROPERTIES 24u
typedef struct {
    char name[64];
    uint64_t value;
} Property;
typedef struct {
    bool used;
    Property items[MAX_PROPERTIES];
    unsigned count;
} PropertySet;
static PropertySet property_sets[MAX_PROPERTY_SETS];
static uint32_t sdlCreateProperties(void) {
    for (unsigned i = 1; i < MAX_PROPERTY_SETS; ++i)
        if (!property_sets[i].used) {
            memset(&property_sets[i], 0, sizeof(property_sets[i]));
            property_sets[i].used = true;
            return i;
        }
    return 0;
}
static void sdlDestroyProperties(uint32_t id) {
    if (id && id < MAX_PROPERTY_SETS) property_sets[id].used = false;
}
static Property *findProperty(uint32_t id, const char *name, bool create) {
    if (!id || id >= MAX_PROPERTY_SETS || !property_sets[id].used || !name) return NULL;
    PropertySet *set = &property_sets[id];
    for (unsigned i = 0; i < set->count; ++i)
        if (!strcmp(set->items[i].name, name)) return &set->items[i];
    if (!create || set->count == MAX_PROPERTIES || strlen(name) >= sizeof(set->items[0].name)) return NULL;
    Property *item = &set->items[set->count++];
    strcpy(item->name, name);
    return item;
}
static bool sdlSetNumberProperty(uint32_t id, const char *name, int64_t value) {
    Property *p = findProperty(id, name, true);
    if (p) p->value = (uint64_t)value;
    return p != NULL;
}
static bool sdlSetPointerProperty(uint32_t id, const char *name, void *value) {
    Property *p = findProperty(id, name, true);
    if (p) p->value = (uintptr_t)value;
    return p != NULL;
}
static bool sdlSetBooleanProperty(uint32_t id, const char *name, bool value) {
    Property *p = findProperty(id, name, true);
    if (p) p->value = value;
    return p != NULL;
}
static bool sdlSetStringProperty(uint32_t id, const char *name, const char *value) {
    Property *p = findProperty(id, name, true);
    if (p) p->value = (uintptr_t)value;
    return p != NULL;
}
static int64_t sdlGetNumberProperty(uint32_t id, const char *name, int64_t fallback) {
    Property *p = findProperty(id, name, false);
    return p ? (int64_t)p->value : fallback;
}
static void *sdlGetPointerProperty(uint32_t id, const char *name, void *fallback) {
    Property *p = findProperty(id, name, false);
    return p ? (void *)(uintptr_t)p->value : fallback;
}
static bool sdlGetBooleanProperty(uint32_t id, const char *name, bool fallback) {
    Property *p = findProperty(id, name, false);
    return p ? p->value != 0 : fallback;
}

// ---- displays and windows -----------------------------------------------------------------------------------------------------
typedef struct {
    int x, y, w, h;
} Rect;
typedef struct {
    uint32_t display, format;
    int w, h;
    float density, refresh;
    int numerator, denominator;
    void *internal;
} DisplayMode;
_Static_assert(sizeof(DisplayMode) == 40, "SDL_DisplayMode");
#define PIXELFORMAT_XRGB8888 0x16161804u
static DisplayMode display_mode;
static void screenSize(unsigned *width, unsigned *height) {
    NWindow *window = nwindowGetDefault();
    u32 w = 0, h = 0;
    if (R_FAILED(nwindowGetDimensions(window, &w, &h)) || !w || !h) {
        w = 1280;
        h = 720;
    }
    *width = w;
    *height = h;
}
static void refreshMode(void) {
    unsigned w, h;
    screenSize(&w, &h);
    display_mode = (DisplayMode){1, PIXELFORMAT_XRGB8888, (int)w, (int)h, 1.0f, 60.0f, 60, 1, NULL};
}
static int sdlGetNumVideoDrivers(void) { return 1; }
static const char *sdlGetVideoDriver(int index) { return index == 0 ? "x11" : NULL; }  // the game only accepts the Linux compositors
static const char *sdlGetCurrentVideoDriver(void) { return "x11"; }
static uint32_t *sdlGetDisplays(int *count) {
    uint32_t *list = linuxAbiCalloc(2, sizeof(uint32_t));
    if (list) list[0] = 1;
    if (count) *count = list ? 1 : 0;
    return list;
}
static uint32_t sdlGetPrimaryDisplay(void) { return 1; }
static const char *sdlGetDisplayName(uint32_t display) {
    (void)display;
    return "Nintendo Switch";
}
static bool sdlGetDisplayBounds(uint32_t display, Rect *rect) {
    (void)display;
    unsigned w, h;
    screenSize(&w, &h);
    if (rect) *rect = (Rect){0, 0, (int)w, (int)h};
    return true;
}
static const DisplayMode *sdlGetDisplayMode(uint32_t display) {
    (void)display;
    refreshMode();
    return &display_mode;
}
static DisplayMode **sdlGetFullscreenDisplayModes(uint32_t display, int *count) {
    (void)display;
    refreshMode();
    DisplayMode **list = linuxAbiCalloc(2, sizeof(DisplayMode *));
    if (list) list[0] = &display_mode;
    if (count) *count = list ? 1 : 0;
    return list;
}
static uint32_t sdlGetDisplayForWindow(void *window) {
    (void)window;
    return 1;
}

typedef struct {
    bool used;
    uint32_t id;
    int w, h;
    uint64_t flags;
    char title[64];
} Window;
#define MAX_WINDOWS 4u
static Window windows[MAX_WINDOWS];
static unsigned windows_created, next_window_id = 1;
static void *newWindow(const char *title, int w, int h, uint64_t flags) {
    (void)w;
    (void)h;
    for (unsigned i = 0; i < MAX_WINDOWS; ++i)
        if (!windows[i].used) {
            unsigned width, height;
            screenSize(&width, &height);
            windows[i] = (Window){true, next_window_id++, (int)width, (int)height, flags, {0}};
            snprintf(windows[i].title, sizeof(windows[i].title), "%s", title ? title : "");
            ++windows_created;
            trace("sdl.window id=%u requested=%dx%d actual=%ux%u flags=0x%llx title=%s", windows[i].id, w, h, width, height, (unsigned long long)flags,
                  windows[i].title);
            return &windows[i];
        }
    return NULL;
}
static void *sdlCreateWindow(const char *title, int w, int h, uint64_t flags) { return newWindow(title, w, h, flags); }
static void *sdlCreateWindowWithProperties(uint32_t properties) {
    const char *title = sdlGetPointerProperty(properties, "SDL_PROP_WINDOW_CREATE_TITLE_STRING", NULL);
    return newWindow(title, (int)sdlGetNumberProperty(properties, "SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER", 1280),
                     (int)sdlGetNumberProperty(properties, "SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER", 720), 0x2);
}
static Window *checked(void *handle) {
    Window *w = handle;
    return (w >= windows && w < windows + MAX_WINDOWS && w->used) ? w : NULL;
}
static void sdlDestroyWindow(void *handle) {
    Window *w = checked(handle);
    if (w) w->used = false;
}
static bool sdlGetWindowSize(void *handle, int *width, int *height) {
    Window *w = checked(handle);
    unsigned sw, sh;
    screenSize(&sw, &sh);
    if (w) {
        w->w = (int)sw;
        w->h = (int)sh;
    }
    if (width) *width = (int)sw;
    if (height) *height = (int)sh;
    return true;
}
static uint32_t sdlGetWindowID(void *handle) {
    Window *w = checked(handle);
    return w ? w->id : 0;
}
static void *sdlGetWindowFromID(uint32_t id) {
    for (unsigned i = 0; i < MAX_WINDOWS; ++i)
        if (windows[i].used && windows[i].id == id) return &windows[i];
    return NULL;
}
static uint64_t sdlGetWindowFlags(void *handle) {
    Window *w = checked(handle);
    return (w ? w->flags : 0) | 0x2 /* OPENGL */ | 0x8000000 /* INPUT_FOCUS-like */;
}
static bool sdlGetWindowPosition(void *handle, int *x, int *y) {
    (void)handle;
    if (x) *x = 0;
    if (y) *y = 0;
    return true;
}
static bool sdlGetWindowSafeArea(void *handle, Rect *rect) {
    int w, h;
    sdlGetWindowSize(handle, &w, &h);
    if (rect) *rect = (Rect){0, 0, w, h};
    return true;
}
static bool sdlGetWindowBordersSize(void *handle, int *top, int *left, int *bottom, int *right) {
    (void)handle;
    if (top) *top = 0;
    if (left) *left = 0;
    if (bottom) *bottom = 0;
    if (right) *right = 0;
    return true;
}
// The window is the console's screen: the game's display settings are accepted and have no effect. They are written to the log (a few
// of each) so that a diagnostics file shows which of them the game really asks for.
static unsigned window_requests;
static void windowRequest(const char *name, long a, long b) {
    if (window_requests++ < 40) trace("sdl.%s %ld %ld (accepted, no effect: the window is the screen)", name, a, b);
}
static bool sdlSetWindowFullscreen(void *handle, bool fullscreen) {
    (void)handle;
    windowRequest("SetWindowFullscreen", fullscreen, 0);
    return true;
}
static bool sdlSetWindowSize(void *handle, int w, int h) {
    (void)handle;
    windowRequest("SetWindowSize", w, h);
    return true;
}
static bool sdlSetWindowBordered(void *handle, bool bordered) {
    (void)handle;
    windowRequest("SetWindowBordered", bordered, 0);
    return true;
}
static bool sdlSetWindowResizable(void *handle, bool resizable) {
    (void)handle;
    windowRequest("SetWindowResizable", resizable, 0);
    return true;
}
static bool sdlSetWindowFullscreenMode(void *handle, const DisplayMode *mode) {
    (void)handle;
    windowRequest("SetWindowFullscreenMode", mode ? mode->w : 0, mode ? mode->h : 0);
    return true;
}
static bool sdlShowSimpleMessageBox(uint32_t flags, const char *title, const char *message, void *window) {
    (void)flags;
    (void)window;
    trace("sdl.MESSAGEBOX title=%s message=%s", title ? title : "", message ? message : "");
    return true;
}

// ---- surfaces and cursors (the icon and the mouse cursors are accepted and dropped) -------------------------------------
typedef struct {
    uint32_t flags, format;
    int w, h, pitch;
    void *pixels;
    int refcount;
    void *reserved;
} Surface;
_Static_assert(sizeof(Surface) == 48, "SDL_Surface");
static int pitchFor(uint32_t format, int width) {
    (void)format;
    return width * 4;
}
static Surface *sdlCreateSurface(int w, int h, uint32_t format) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return NULL;
    Surface *surface = linuxAbiCalloc(1, sizeof(*surface));
    if (!surface) return NULL;
    surface->format = format;
    surface->w = w;
    surface->h = h;
    surface->pitch = pitchFor(format, w);
    surface->refcount = 1;
    surface->pixels = linuxAbiCalloc(1, (size_t)surface->pitch * (size_t)h);
    if (!surface->pixels) {
        linuxAbiFree(surface);
        return NULL;
    }
    return surface;
}
static Surface *sdlCreateSurfaceFrom(int w, int h, uint32_t format, void *pixels, int pitch) {
    Surface *surface = linuxAbiCalloc(1, sizeof(*surface));
    if (!surface) return NULL;
    surface->format = format;
    surface->w = w;
    surface->h = h;
    surface->pitch = pitch;
    surface->pixels = pixels;
    surface->refcount = 1;
    surface->flags = 0x2;  // PREALLOCATED
    return surface;
}
static void sdlDestroySurface(Surface *surface) {
    if (!surface) return;
    if (!(surface->flags & 0x2)) linuxAbiFree(surface->pixels);
    linuxAbiFree(surface);
}
static void *sdlCreateSystemCursor(int id) {
    static char cursors[32][8];
    return id >= 0 && id < 32 ? cursors[id] : NULL;
}

// ---- OpenGL through EGL (the context and surface of the console's window) -----------------------------------------------
static int gl_attributes[48];
static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLSurface egl_surface = EGL_NO_SURFACE;
static EGLContext egl_context = EGL_NO_CONTEXT;
static EGLConfig egl_config;
static _Atomic unsigned requested_size;  // width << 16 | height, 0: nothing asked
static unsigned contexts_created, swap_interval = 1;
static bool sdlGlSetAttribute(int attribute, int value) {
    if (attribute >= 0 && attribute < 48) gl_attributes[attribute] = value;
    trace("sdl.GL_SetAttribute %d=%d", attribute, value);
    return true;
}
static bool sdlGlGetAttribute(int attribute, int *value) {
    if (value) *value = attribute >= 0 && attribute < 48 ? gl_attributes[attribute] : 0;
    return true;
}
static void sdlGlResetAttributes(void) { memset(gl_attributes, 0, sizeof(gl_attributes)); }
static void *sdlGlCreateContext(void *window) {
    (void)window;
    NWindow *native = nwindowGetDefault();
    const EGLint config_attributes[] = {EGL_RENDERABLE_TYPE,
                                        EGL_OPENGL_BIT,
                                        EGL_SURFACE_TYPE,
                                        EGL_WINDOW_BIT,
                                        EGL_RED_SIZE,
                                        8,
                                        EGL_GREEN_SIZE,
                                        8,
                                        EGL_BLUE_SIZE,
                                        8,
                                        EGL_ALPHA_SIZE,
                                        8,
                                        EGL_DEPTH_SIZE,
                                        gl_attributes[6] ? gl_attributes[6] : 16,
                                        EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_display == EGL_NO_DISPLAY || !eglInitialize(egl_display, NULL, NULL)) {
        trace("sdl.GL_CreateContext=FAIL stage=initialize egl_error=0x%x", eglGetError());
        return NULL;
    }
    if (!eglBindAPI(EGL_OPENGL_API) || !eglChooseConfig(egl_display, config_attributes, &config, 1, &count) || count < 1) {
        trace("sdl.GL_CreateContext=FAIL stage=config egl_error=0x%x", eglGetError());
        return NULL;
    }
    egl_config = config;
    egl_surface = eglCreateWindowSurface(egl_display, config, (EGLNativeWindowType)native, NULL);
    if (egl_surface == EGL_NO_SURFACE) {
        trace("sdl.GL_CreateContext=FAIL stage=surface egl_error=0x%x", eglGetError());
        return NULL;
    }
    int major = gl_attributes[17] ? gl_attributes[17] : 2, minor = gl_attributes[18];
    const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, major, EGL_CONTEXT_MINOR_VERSION_KHR, minor ? minor : 1, EGL_NONE};
    egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attributes);
    if (egl_context == EGL_NO_CONTEXT) {
        trace("sdl.GL_CreateContext=FAIL stage=context egl_error=0x%x", eglGetError());
        return NULL;
    }
    if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
        trace("sdl.GL_CreateContext=FAIL stage=make_current egl_error=0x%x", eglGetError());
        return NULL;
    }
    eglSwapInterval(egl_display, (EGLint)swap_interval);
    ++contexts_created;
    trace("sdl.GL_CreateContext=PASS version=%d.%d context=%p", major, minor ? minor : 1, egl_context);
    return egl_context;
}
static bool sdlGlMakeCurrent(void *window, void *context) {
    (void)window;
    if (egl_display == EGL_NO_DISPLAY) return false;
    if (!context) return eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return eglMakeCurrent(egl_display, egl_surface, egl_surface, (EGLContext)context);
}
static void *sdlGlGetCurrentContext(void) { return eglGetCurrentContext() == EGL_NO_CONTEXT ? NULL : eglGetCurrentContext(); }
static bool sdlGlDestroyContext(void *context) {
    (void)context;
    return true;
}  // torn down by linuxSdlReset
static bool sdlGlSetSwapInterval(int interval) {
    trace("sdl.GL_SetSwapInterval %d", interval);
    swap_interval = interval > 0 ? (unsigned)interval : 0;
    return egl_display == EGL_NO_DISPLAY || eglSwapInterval(egl_display, (EGLint)swap_interval);
}
static bool sdlGlGetSwapInterval(int *interval) {
    if (interval) *interval = (int)swap_interval;
    return true;
}
void linuxSdlRequestSize(unsigned width, unsigned height) { atomic_store(&requested_size, width << 16 | height); }
// The picture changes size when the console is docked or undocked. The surface of a window cannot change size while it is in use, so it is
// replaced (the context and everything the game created in it stay) and the game is told that its window was resized. Done by the thread that
// renders, between two frames. If the new surface cannot be made, the old size is made again.
static void applySizeRequest(void) {
    unsigned request = atomic_exchange(&requested_size, 0), old_width, old_height;
    screenSize(&old_width, &old_height);
    if (!request || (request >> 16 == old_width && (request & 0xffff) == old_height)) return;
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(egl_display, egl_surface);
    egl_surface = EGL_NO_SURFACE;
    for (unsigned attempt = 0; attempt < 2 && egl_surface == EGL_NO_SURFACE; ++attempt) {
        displayPrepareWindow();
        unsigned width = attempt ? old_width : request >> 16, height = attempt ? old_height : request & 0xffff;
        Result rc = nwindowSetDimensions(nwindowGetDefault(), width, height);
        egl_surface = eglCreateWindowSurface(egl_display, egl_config, (EGLNativeWindowType)nwindowGetDefault(), NULL);
        trace("sdl.resize %ux%u window_rc=0x%x surface=%s", width, height, rc, egl_surface == EGL_NO_SURFACE ? "FAIL" : "OK");
    }
    if (egl_surface == EGL_NO_SURFACE || !eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
        trace("sdl.resize=FAIL egl_error=0x%x", eglGetError());
        return;
    }
    eglSwapInterval(egl_display, (EGLint)swap_interval);
    unsigned width, height;
    screenSize(&width, &height);
    linuxSdlEventsWindowResized(width, height, armTicksToNs(armGetSystemTick()));
}
static bool sdlGlSwapWindow(void *window) {
    (void)window;
    if (egl_display == EGL_NO_DISPLAY || egl_surface == EGL_NO_SURFACE) return false;
    if (linuxFilePickerOpen()) {
        unsigned w, h;
        screenSize(&w, &h);
        linuxFilePickerDraw(w, h);
    } else {
        unsigned w, h;
        screenSize(&w, &h);
        linuxSdlCursorDraw(w, h);
    }  // the file chooser or the cursor, over the frame just drawn
    bool ok = eglSwapBuffers(egl_display, egl_surface);
    if (ok) applySizeRequest();
    unsigned frame = atomic_fetch_add(&frame_counter, 1) + 1;
    if (frame == 1 || frame == 2 || frame == 60 || frame == 600 || !ok) trace("sdl.GL_SwapWindow frame=%u result=%s", frame, ok ? "OK" : "FAIL");
    return ok;
}
static void *sdlGlGetProcAddress(const char *name) { return name ? (void *)eglGetProcAddress(name) : NULL; }
static bool sdlGlExtensionSupported(const char *name) {
    (void)name;
    return false;
}

// ---- events, input, time ----------------------------------------------------------------------------------------------------------
// The console's controller and touch screen are sampled when the game pumps events (at most once per millisecond) and
// turned into SDL events by linux_sdl_events.c: touch is a mouse, the controller is gamepad number 1.
static atomic_ullong last_pump_ns;
static void pumpInput(void) {
    linuxSdlInputKeyboardPump();
    uint64_t now = armTicksToNs(armGetSystemTick());
    uint64_t before = atomic_load(&last_pump_ns);
    if (now - before < 1000000ull || !atomic_compare_exchange_strong(&last_pump_ns, &before, now)) return;
    LinuxInputSnapshot snapshot;
    static bool keyboard_button_before;
    if (linuxSdlInputSample(&snapshot)) {
        if (linuxFilePickerCapturesInput()) {
            // The file chooser owns the controller and the touch screen: the game sees them idle, so that nothing stays pressed behind it.
            linuxFilePickerFeed(&snapshot);
            LinuxInputSnapshot idle = {0};
            idle.gamepad = snapshot.gamepad;
            idle.timestamp_ns = snapshot.timestamp_ns;
            linuxSdlEventsUpdate(&idle);
            keyboard_button_before = true;
            return;
        }
        // Clicking the right stick brings the keyboard up or down, by hand, whatever the game is doing: the game's own requests for text
        // input are not followed (they only came for some fields).
        bool keyboard_button = snapshot.gamepad && ((snapshot.buttons >> 8) & 1u);  // SDL_GAMEPAD_BUTTON_RIGHT_STICK
        if (keyboard_button && !keyboard_button_before && linuxSdlInputKeyboardAvailable()) {
            bool show = !linuxSdlInputKeyboardVisible();
            trace("sdl.keyboard.toggle show=%d", show);
            linuxSdlInputKeyboardRequest(show);
        }
        keyboard_button_before = keyboard_button;
        snapshot.buttons &= ~(1u << 8);  // the keyboard's button is not also a game button
        unsigned width, height;
        screenSize(&width, &height);
        linuxSdlCursorUpdate(&snapshot, width, height);
        snapshot.x *= (float)width / 1280.0f;  // the touch screen reports positions of a 1280x720 screen, the mouse of the game lives in window pixels
        snapshot.y *= (float)height / 720.0f;
        linuxSdlEventsUpdate(&snapshot);
    }
}
bool linuxSdlThreadHasContext(void) { return egl_context != EGL_NO_CONTEXT && eglGetCurrentContext() != EGL_NO_CONTEXT; }
bool linuxSdlPresentFrame(void) {
    pumpInput();
    return sdlGlSwapWindow(NULL);
}
static void sdlPumpEvents(void) { pumpInput(); }
static void sdlUpdateJoysticks(void) { pumpInput(); }
static bool sdlPollEvent(void *event) {
    pumpInput();
    if (!event) return linuxSdlEventsPending() > 0 || atomic_load(&stop_requested);
    if (atomic_exchange(&stop_requested, false)) {
        memset(event, 0, 128);
        *(uint32_t *)event = LINUX_SDL_EVENT_QUIT;
        return true;
    }
    return linuxSdlEventsPop(event);
}
// Links ("open in the browser"): the console has no browser to give, the request is refused and its address written to the log.
static bool sdlOpenUrl(const char *url) {
    static atomic_uint requests;
    if (atomic_fetch_add(&requests, 1) < 20) trace("sdl.OpenURL url=%.200s (refused: no browser)", url ? url : "(none)");
    return false;
}
// Memory streams: the game hands its gamepad database to SDL through one. The mappings are counted and ignored (the controller is described by this project).
static void *sdlIoFromMem(void *memory, size_t size) { return linuxSdlIoFromMemory(memory, size, true); }
static void *sdlIoFromConstMem(const void *memory, size_t size) { return linuxSdlIoFromMemory((void *)memory, size, false); }
static size_t sdlReadIo(void *io, void *buffer, size_t size) { return linuxSdlIoRead(io, buffer, size); }
static size_t sdlWriteIo(void *io, const void *buffer, size_t size) { return linuxSdlIoWrite(io, buffer, size); }
static int64_t sdlSeekIo(void *io, int64_t offset, int whence) { return linuxSdlIoSeek(io, offset, whence); }
static int64_t sdlTellIo(void *io) { return linuxSdlIoTell(io); }
static int64_t sdlGetIoSize(void *io) { return linuxSdlIoSize(io); }
static bool sdlCloseIo(void *io) { return linuxSdlIoClose(io); }
static int sdlAddGamepadMappingsFromIo(void *io, bool close_io) {
    int count = linuxSdlIoCountMappings(io);
    trace("sdl.gamepad.mappings count=%d (accepted, not used)", count);
    if (close_io) linuxSdlIoClose(io);
    return count;
}
static int sdlAddGamepadMapping(const char *mapping) {
    (void)mapping;
    return 0;
}
static int sdlAddGamepadMappingsFromFile(const char *file) {
    (void)file;
    return 0;
}
static void sdlUpdateGamepads(void) { pumpInput(); }
static int sdlJoystickPowerInfo(void *joystick, int *percent) {
    (void)joystick;
    if (percent) *percent = -1;
    return 0;
}  // SDL_POWERSTATE_UNKNOWN
static uint16_t sdlJoystickVendorForId(uint32_t id) {
    (void)id;
    return 0x057e;
}
static uint16_t sdlJoystickProductForId(uint32_t id) {
    (void)id;
    return 0x2009;
}
static void *sdlCreateColorCursor(void *surface, int hot_x, int hot_y) {
    (void)surface;
    (void)hot_x;
    (void)hot_y;
    static char cursor[8];
    return cursor;
}
static uint32_t sdlGetKeyFromScancode(int scancode, uint16_t modifiers, bool key_event) {
    (void)modifiers;
    (void)key_event;
    return scancode < 0 ? 0 : linuxSdlKeyFromScancode((uint32_t)scancode);
}
static const char *sdlGetKeyName(uint32_t key) { return linuxSdlKeyName(key); }
// Text fields: the game's requests are acknowledged and nothing more; the keyboard is the right stick button's (see pumpInput).
static bool sdlStartTextInput(void *window) {
    (void)window;
    trace("sdl.StartTextInput");
    return true;
}
static bool sdlStartTextInputWithProperties(void *window, uint32_t properties) {
    (void)properties;
    return sdlStartTextInput(window);
}
static bool sdlStopTextInput(void *window) {
    (void)window;
    trace("sdl.StopTextInput");
    return true;
}
static bool sdlTextInputActive(void *window) {
    (void)window;
    return linuxSdlInputKeyboardVisible();
}
static uint8_t keyboard_state[512];
static const uint8_t *sdlGetKeyboardState(int *count) {
    if (count) *count = 512;
    return keyboard_state;
}
static uint32_t sdlGetMouseState(float *x, float *y) {
    pumpInput();
    return linuxSdlEventsMouse(x, y);
}
static uint32_t *sdlEmptyIdList(int *count) {
    if (count) *count = 0;
    return linuxAbiCalloc(2, sizeof(uint32_t));
}
// gamepads: one controller, id 1, while the console reports one
static uint32_t *sdlGamepadIds(int *count) {
    pumpInput();
    int present = linuxSdlEventsGamepadCount();
    if (count) *count = present;
    uint32_t *ids = linuxAbiCalloc(2, sizeof(uint32_t));
    if (ids && present) ids[0] = LINUX_SDL_GAMEPAD_ID;
    return ids;
}
static char gamepad_object[64], joystick_object[64];
static bool sdlIsGamepad(uint32_t id) { return id == LINUX_SDL_GAMEPAD_ID && linuxSdlEventsGamepadCount() > 0; }
static void *sdlOpenGamepad(uint32_t id) {
    trace("sdl.OpenGamepad id=%u", id);
    return sdlIsGamepad(id) ? gamepad_object : NULL;
}
static void *sdlGetGamepadFromId(uint32_t id) { return sdlIsGamepad(id) ? gamepad_object : NULL; }
static void *sdlGetJoystickFromId(uint32_t id) { return sdlIsGamepad(id) ? joystick_object : NULL; }
static void *sdlGetGamepadJoystick(void *gamepad) {
    (void)gamepad;
    return joystick_object;
}
static uint32_t sdlGamepadId(void *object) {
    (void)object;
    return LINUX_SDL_GAMEPAD_ID;
}
// Short on purpose: with a longer name the Android theme of the game loops its layout on the settings pages (lag, and a warning).
#define GAMEPAD_NAME "Switch Controller"
static const char *sdlGamepadName(void *object) {
    (void)object;
    return GAMEPAD_NAME;
}
static const char *sdlGamepadNameForId(uint32_t id) {
    (void)id;
    return GAMEPAD_NAME;
}
static int sdlGamepadType(void *object) {
    (void)object;
    return 7;
}  // SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO
static int sdlGamepadTypeForId(uint32_t id) {
    (void)id;
    return 7;
}
static int sdlJoystickType(void *object) {
    (void)object;
    return 1;
}  // SDL_JOYSTICK_TYPE_GAMEPAD
static uint16_t sdlGamepadVendor(void *object) {
    (void)object;
    return 0x057e;
}
static uint16_t sdlGamepadProduct(void *object) {
    (void)object;
    return 0x2009;
}
static uint16_t sdlGamepadZero16(void *object) {
    (void)object;
    return 0;
}
static int sdlGamepadZeroInt(void *object) {
    (void)object;
    return 0;
}
static bool sdlGamepadButton(void *object, int button) {
    (void)object;
    return linuxSdlEventsGamepadButton(button);
}
static int16_t sdlGamepadAxis(void *object, int axis) {
    (void)object;
    return linuxSdlEventsGamepadAxis(axis);
}
static bool sdlGamepadHasButton(void *object, int button) {
    (void)object;
    return button >= 0 && button < LINUX_SDL_GAMEPAD_BUTTONS;
}
static bool sdlGamepadHasAxis(void *object, int axis) {
    (void)object;
    return axis >= 0 && axis < LINUX_SDL_GAMEPAD_AXES;
}
static bool sdlHasGamepad(void) { return linuxSdlEventsGamepadCount() > 0; }
static bool sdlGamepadConnected(void *object) {
    (void)object;
    return linuxSdlEventsGamepadCount() > 0;
}
static uint64_t ticksNs(void) { return armTicksToNs(armGetSystemTick()); }
// The game waits for the next frame by reading the clock in a loop (millions of calls): every few calls yield the core so that
// the other threads (audio, loading, network) are not kept waiting by it.
static atomic_uint clock_reads;
static void clockYield(void) {
    if ((atomic_fetch_add_explicit(&clock_reads, 1, memory_order_relaxed) & 31u) == 31u) svcSleepThread(0);
}
static uint64_t sdlGetTicks(void) {
    clockYield();
    return ticksNs() / 1000000u;
}
static uint64_t sdlGetTicksNs(void) {
    clockYield();
    return ticksNs();
}
static uint64_t sdlGetPerformanceCounter(void) { return ticksNs(); }
static uint64_t sdlGetPerformanceFrequency(void) { return 1000000000u; }
static void sdlDelay(uint32_t milliseconds) { svcSleepThread((s64)milliseconds * 1000000); }
static void sdlDelayNs(uint64_t nanoseconds) { svcSleepThread((s64)nanoseconds); }

// ---- function table ------------------------------------------------------------------------------------------------------------------
#define ENTRY(name, function) {name, (uintptr_t)(function)}
typedef struct {
    const char *name;
    uintptr_t address;
} Entry;
static const Entry sdl_table[] = {
    ENTRY("SDL_Init", sdlInit),
    ENTRY("SDL_InitSubSystem", sdlInitSubSystem),
    ENTRY("SDL_QuitSubSystem", sdlVoid),
    ENTRY("SDL_Quit", sdlVoid),
    ENTRY("SDL_WasInit", sdlWasInit),
    ENTRY("SDL_GetError", sdlGetError),
    ENTRY("SDL_ClearError", sdlClearError),
    ENTRY("SDL_SetError", sdlSetError),
    ENTRY("SDL_SetHint", sdlSetHint),
    ENTRY("SDL_SetHintWithPriority", sdlSetHintWithPriority),
    ENTRY("SDL_GetHint", sdlNull),
    ENTRY("SDL_SetAppMetadata", sdlTrue),
    ENTRY("SDL_SetAppMetadataProperty", sdlTrue),
    ENTRY("SDL_GetVersion", sdlGetVersion),
    ENTRY("SDL_GetRevision", sdlGetRevision),
    ENTRY("SDL_GetPlatform", sdlGetPlatform),
    ENTRY("SDL_malloc", sdlMalloc),
    ENTRY("SDL_calloc", sdlCalloc),
    ENTRY("SDL_realloc", sdlRealloc),
    ENTRY("SDL_free", sdlFree),
    ENTRY("SDL_SetLogOutputFunction", sdlVoid),
    ENTRY("SDL_SetLogPriority", sdlVoid),
    ENTRY("SDL_CreateProperties", sdlCreateProperties),
    ENTRY("SDL_DestroyProperties", sdlDestroyProperties),
    ENTRY("SDL_SetNumberProperty", sdlSetNumberProperty),
    ENTRY("SDL_SetPointerProperty", sdlSetPointerProperty),
    ENTRY("SDL_SetBooleanProperty", sdlSetBooleanProperty),
    ENTRY("SDL_SetStringProperty", sdlSetStringProperty),
    ENTRY("SDL_GetNumberProperty", sdlGetNumberProperty),
    ENTRY("SDL_GetPointerProperty", sdlGetPointerProperty),
    ENTRY("SDL_GetBooleanProperty", sdlGetBooleanProperty),
    ENTRY("SDL_GetNumVideoDrivers", sdlGetNumVideoDrivers),
    ENTRY("SDL_GetVideoDriver", sdlGetVideoDriver),
    ENTRY("SDL_GetCurrentVideoDriver", sdlGetCurrentVideoDriver),
    ENTRY("SDL_GetSystemTheme", sdlFalse),
    ENTRY("SDL_GetDisplays", sdlGetDisplays),
    ENTRY("SDL_GetPrimaryDisplay", sdlGetPrimaryDisplay),
    ENTRY("SDL_GetDisplayName", sdlGetDisplayName),
    ENTRY("SDL_GetDisplayBounds", sdlGetDisplayBounds),
    ENTRY("SDL_GetDisplayUsableBounds", sdlGetDisplayBounds),
    ENTRY("SDL_GetDisplayContentScale", sdlOneFloat),
    ENTRY("SDL_GetDesktopDisplayMode", sdlGetDisplayMode),
    ENTRY("SDL_GetCurrentDisplayMode", sdlGetDisplayMode),
    ENTRY("SDL_GetFullscreenDisplayModes", sdlGetFullscreenDisplayModes),
    ENTRY("SDL_GetDisplayForWindow", sdlGetDisplayForWindow),
    ENTRY("SDL_GetNaturalDisplayOrientation", sdlFalse),
    ENTRY("SDL_GetCurrentDisplayOrientation", sdlFalse),
    ENTRY("SDL_CreateWindow", sdlCreateWindow),
    ENTRY("SDL_CreateWindowWithProperties", sdlCreateWindowWithProperties),
    ENTRY("SDL_DestroyWindow", sdlDestroyWindow),
    ENTRY("SDL_GetWindowSize", sdlGetWindowSize),
    ENTRY("SDL_GetWindowSizeInPixels", sdlGetWindowSize),
    ENTRY("SDL_SetWindowSize", sdlSetWindowSize),
    ENTRY("SDL_GetWindowPosition", sdlGetWindowPosition),
    ENTRY("SDL_GetWindowID", sdlGetWindowID),
    ENTRY("SDL_GetWindowFromID", sdlGetWindowFromID),
    ENTRY("SDL_GetWindowFlags", sdlGetWindowFlags),
    ENTRY("SDL_GetWindowDisplayScale", sdlOneFloat),
    ENTRY("SDL_GetWindowPixelDensity", sdlOneFloat),
    ENTRY("SDL_GetWindowSafeArea", sdlGetWindowSafeArea),
    ENTRY("SDL_GetWindowBordersSize", sdlGetWindowBordersSize),
    ENTRY("SDL_GetWindowFullscreenMode", sdlNull),
    ENTRY("SDL_GetWindowProperties", sdlFalse),
    ENTRY("SDL_SetWindowTitle", sdlTrue),
    ENTRY("SDL_SetWindowIcon", sdlTrue),
    ENTRY("SDL_ShowWindow", sdlTrue),
    ENTRY("SDL_HideWindow", sdlTrue),
    ENTRY("SDL_RaiseWindow", sdlTrue),
    ENTRY("SDL_SetWindowPosition", sdlTrue),
    ENTRY("SDL_SetWindowMinimumSize", sdlTrue),
    ENTRY("SDL_SetWindowMaximumSize", sdlTrue),
    ENTRY("SDL_MaximizeWindow", sdlTrue),
    ENTRY("SDL_SetWindowFullscreen", sdlSetWindowFullscreen),
    ENTRY("SDL_SetWindowFullscreenMode", sdlSetWindowFullscreenMode),
    ENTRY("SDL_SetWindowResizable", sdlSetWindowResizable),
    ENTRY("SDL_SetWindowBordered", sdlSetWindowBordered),
    ENTRY("SDL_SyncWindow", sdlTrue),
    ENTRY("SDL_StartTextInput", sdlStartTextInput),
    ENTRY("SDL_StartTextInputWithProperties", sdlStartTextInputWithProperties),
    ENTRY("SDL_StopTextInput", sdlStopTextInput),
    ENTRY("SDL_TextInputActive", sdlTextInputActive),
    ENTRY("SDL_SetTextInputArea", sdlTrue),
    ENTRY("SDL_ClearComposition", sdlTrue),
    ENTRY("SDL_IOFromMem", sdlIoFromMem),
    ENTRY("SDL_IOFromConstMem", sdlIoFromConstMem),
    ENTRY("SDL_ReadIO", sdlReadIo),
    ENTRY("SDL_WriteIO", sdlWriteIo),
    ENTRY("SDL_SeekIO", sdlSeekIo),
    ENTRY("SDL_TellIO", sdlTellIo),
    ENTRY("SDL_GetIOSize", sdlGetIoSize),
    ENTRY("SDL_CloseIO", sdlCloseIo),
    ENTRY("SDL_AddGamepadMappingsFromIO", sdlAddGamepadMappingsFromIo),
    ENTRY("SDL_AddGamepadMapping", sdlAddGamepadMapping),
    ENTRY("SDL_AddGamepadMappingsFromFile", sdlAddGamepadMappingsFromFile),
    ENTRY("SDL_UpdateGamepads", sdlUpdateGamepads),
    ENTRY("SDL_GetJoystickPowerInfo", sdlJoystickPowerInfo),
    ENTRY("SDL_GetJoystickVendorForID", sdlJoystickVendorForId),
    ENTRY("SDL_GetJoystickProductForID", sdlJoystickProductForId),
    ENTRY("SDL_CreateColorCursor", sdlCreateColorCursor),
    ENTRY("SDL_GetKeyFromScancode", sdlGetKeyFromScancode),
    ENTRY("SDL_GetKeyName", sdlGetKeyName),
    ENTRY("SDL_OpenURL", sdlOpenUrl),
    ENTRY("SDL_ShowCursor", sdlTrue),
    ENTRY("SDL_HideCursor", sdlTrue),
    ENTRY("SDL_CreateSystemCursor", sdlCreateSystemCursor),
    ENTRY("SDL_SetCursor", sdlTrue),
    ENTRY("SDL_DestroyCursor", sdlVoid),
    ENTRY("SDL_SetWindowMouseGrab", sdlTrue),
    ENTRY("SDL_SetWindowKeyboardGrab", sdlTrue),
    ENTRY("SDL_SetWindowRelativeMouseMode", sdlTrue),
    ENTRY("SDL_ShowSimpleMessageBox", sdlShowSimpleMessageBox),
    ENTRY("SDL_ShowMessageBox", sdlTrue),
    ENTRY("SDL_CreateSurface", sdlCreateSurface),
    ENTRY("SDL_CreateSurfaceFrom", sdlCreateSurfaceFrom),
    ENTRY("SDL_DestroySurface", sdlDestroySurface),
    ENTRY("SDL_AddSurfaceAlternateImage", sdlTrue),
    ENTRY("SDL_GL_SetAttribute", sdlGlSetAttribute),
    ENTRY("SDL_GL_GetAttribute", sdlGlGetAttribute),
    ENTRY("SDL_GL_ResetAttributes", sdlGlResetAttributes),
    ENTRY("SDL_GL_LoadLibrary", sdlTrue),
    ENTRY("SDL_GL_CreateContext", sdlGlCreateContext),
    ENTRY("SDL_GL_DestroyContext", sdlGlDestroyContext),
    ENTRY("SDL_GL_MakeCurrent", sdlGlMakeCurrent),
    ENTRY("SDL_GL_GetCurrentContext", sdlGlGetCurrentContext),
    ENTRY("SDL_GL_SetSwapInterval", sdlGlSetSwapInterval),
    ENTRY("SDL_GL_GetSwapInterval", sdlGlGetSwapInterval),
    ENTRY("SDL_GL_SwapWindow", sdlGlSwapWindow),
    ENTRY("SDL_GL_GetProcAddress", sdlGlGetProcAddress),
    ENTRY("SDL_EGL_GetProcAddress", sdlGlGetProcAddress),
    ENTRY("SDL_GL_ExtensionSupported", sdlGlExtensionSupported),
    ENTRY("SDL_PumpEvents", sdlPumpEvents),
    ENTRY("SDL_PollEvent", sdlPollEvent),
    ENTRY("SDL_WaitEventTimeout", sdlFalse),
    ENTRY("SDL_PushEvent", sdlTrue),
    ENTRY("SDL_SetEventEnabled", sdlVoid),
    ENTRY("SDL_FlushEvents", sdlVoid),
    ENTRY("SDL_UpdateJoysticks", sdlUpdateJoysticks),
    ENTRY("SDL_GetKeyboardState", sdlGetKeyboardState),
    ENTRY("SDL_GetModState", sdlFalse),
    ENTRY("SDL_GetMouseState", sdlGetMouseState),
    ENTRY("SDL_GetGamepads", sdlGamepadIds),
    ENTRY("SDL_GetJoysticks", sdlGamepadIds),
    ENTRY("SDL_GetSensors", sdlEmptyIdList),
    ENTRY("SDL_GetKeyboards", sdlEmptyIdList),
    ENTRY("SDL_GetMice", sdlEmptyIdList),
    ENTRY("SDL_OpenGamepad", sdlOpenGamepad),
    ENTRY("SDL_CloseGamepad", sdlVoid),
    ENTRY("SDL_IsGamepad", sdlIsGamepad),
    ENTRY("SDL_GetGamepadFromID", sdlGetGamepadFromId),
    ENTRY("SDL_GetJoystickFromID", sdlGetJoystickFromId),
    ENTRY("SDL_GetGamepadJoystick", sdlGetGamepadJoystick),
    ENTRY("SDL_GetGamepadID", sdlGamepadId),
    ENTRY("SDL_GetJoystickID", sdlGamepadId),
    ENTRY("SDL_GetGamepadName", sdlGamepadName),
    ENTRY("SDL_GetJoystickName", sdlGamepadName),
    ENTRY("SDL_GetGamepadNameForID", sdlGamepadNameForId),
    ENTRY("SDL_GetJoystickNameForID", sdlGamepadNameForId),
    ENTRY("SDL_GetGamepadType", sdlGamepadType),
    ENTRY("SDL_GetRealGamepadType", sdlGamepadType),
    ENTRY("SDL_GetGamepadTypeForID", sdlGamepadTypeForId),
    ENTRY("SDL_GetRealGamepadTypeForID", sdlGamepadTypeForId),
    ENTRY("SDL_GetJoystickType", sdlJoystickType),
    ENTRY("SDL_GetGamepadVendor", sdlGamepadVendor),
    ENTRY("SDL_GetJoystickVendor", sdlGamepadVendor),
    ENTRY("SDL_GetGamepadProduct", sdlGamepadProduct),
    ENTRY("SDL_GetJoystickProduct", sdlGamepadProduct),
    ENTRY("SDL_GetGamepadProductVersion", sdlGamepadZero16),
    ENTRY("SDL_GetJoystickProductVersion", sdlGamepadZero16),
    ENTRY("SDL_GetGamepadFirmwareVersion", sdlGamepadZero16),
    ENTRY("SDL_GetGamepadPlayerIndex", sdlGamepadZeroInt),
    ENTRY("SDL_GetJoystickPlayerIndex", sdlGamepadZeroInt),
    ENTRY("SDL_GetGamepadButton", sdlGamepadButton),
    ENTRY("SDL_GetGamepadAxis", sdlGamepadAxis),
    ENTRY("SDL_GamepadHasButton", sdlGamepadHasButton),
    ENTRY("SDL_GamepadHasAxis", sdlGamepadHasAxis),
    ENTRY("SDL_GamepadConnected", sdlGamepadConnected),
    ENTRY("SDL_JoystickConnected", sdlGamepadConnected),
    ENTRY("SDL_RumbleGamepad", sdlFalse),
    ENTRY("SDL_RumbleGamepadTriggers", sdlFalse),
    ENTRY("SDL_HasGamepad", sdlHasGamepad),
    ENTRY("SDL_GetTicks", sdlGetTicks),
    ENTRY("SDL_GetTicksNS", sdlGetTicksNs),
    ENTRY("SDL_GetPerformanceCounter", sdlGetPerformanceCounter),
    ENTRY("SDL_GetPerformanceFrequency", sdlGetPerformanceFrequency),
    ENTRY("SDL_Delay", sdlDelay),
    ENTRY("SDL_DelayNS", sdlDelayNs),
};
static uintptr_t lookupIn(const Entry *table, size_t count, const char *name) {
    atomic_fetch_add(&call_counter, 1);
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(table[i].name, name)) return table[i].address;
    atomic_fetch_add(&unknown_counter, 1);
    if (logged_unknown < 40) {
        ++logged_unknown;
        trace("sdl.lookup.unknown=%s", name);
    }
    return 0;
}
static uintptr_t sdlLookup(const char *name) { return lookupIn(sdl_table, sizeof(sdl_table) / sizeof(sdl_table[0]), name); }
const LinuxVirtualLibrary linuxSdlLibrary = {"libSDL3", sdlLookup, "SDL_", false};

// ---- libEGL: Mesa's own entry points ---------------------------------------------------------------------------------------------
static void *glxGetProcAddress(const char *name) { return name ? (void *)eglGetProcAddress(name) : NULL; }
static uintptr_t eglLookup(const char *name) {
    if (!strcmp(name, "glXGetProcAddress") || !strcmp(name, "glXGetProcAddressARB")) return (uintptr_t)glxGetProcAddress;
    if (!strcmp(name, "eglGetProcAddress")) return (uintptr_t)eglGetProcAddress;
    if (!strncmp(name, "egl", 3) || !strncmp(name, "gl", 2)) return (uintptr_t)eglGetProcAddress(name);
    return 0;
}
const LinuxVirtualLibrary linuxEglLibrary = {"libEGL", eglLookup, NULL, false};
// LWJGL looks for the GLX library first (libGLX.so.0, libGL.so.1, libGL.so) and complains loudly when none exists before it settles for EGL. It
// only asks it for glXGetProcAddress and the GL functions, which are the ones above: the first name it tries gets them.
const LinuxVirtualLibrary linuxGlxLibrary = {"libGLX", eglLookup, NULL, false};

LinuxSdlStats linuxSdlStats(void) {
    unsigned w = 0, h = 0;
    if (egl_surface != EGL_NO_SURFACE) screenSize(&w, &h);
    return (LinuxSdlStats){windows_created,
                           contexts_created,
                           atomic_load(&frame_counter),
                           atomic_load(&call_counter),
                           atomic_load(&unknown_counter),
                           egl_context != EGL_NO_CONTEXT,
                           w,
                           h};
}
bool linuxSdlReset(void) {
    bool ok = true;
    linuxSdlIoReset();
    linuxFilePickerReset();
    linuxAudioReset();  // the game is over: stop the audio output before anything else goes away
    if (egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_context != EGL_NO_CONTEXT && !eglDestroyContext(egl_display, egl_context)) ok = false;
        if (egl_surface != EGL_NO_SURFACE && !eglDestroySurface(egl_display, egl_surface)) ok = false;
        if (!eglTerminate(egl_display)) ok = false;
    }
    egl_display = EGL_NO_DISPLAY;
    egl_surface = EGL_NO_SURFACE;
    egl_context = EGL_NO_CONTEXT;
    memset(windows, 0, sizeof(windows));
    memset(property_sets, 0, sizeof(property_sets));
    memset(gl_attributes, 0, sizeof(gl_attributes));
    windows_created = contexts_created = 0;
    next_window_id = 1;
    logged_unknown = 0;
    atomic_store(&frame_counter, 0);
    atomic_store(&call_counter, 0);
    atomic_store(&unknown_counter, 0);
    atomic_store(&stop_requested, false);
    atomic_store(&requested_size, 0);
    linuxSdlEventsReset();
    atomic_store(&last_pump_ns, 0);
    return ok;
}
