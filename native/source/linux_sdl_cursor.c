#include "linux_sdl_cursor.h"
#include "diagnostics.h"
#include <EGL/egl.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#define LEFT_STICK_BUTTON 7  // SDL_GAMEPAD_BUTTON_LEFT_STICK
#define DEAD_ZONE 0.15f
#define TRIGGER_PRESSED 16000
#define SCREENS_PER_SECOND 1.2f  // speed of the cursor, in screen heights, with the stick at its limit; slower below, by the square
#define LONGEST_STEP_SECONDS 0.1f

static bool active, toggle_before;
static float cursor_x, cursor_y;
static unsigned last_width, last_height;
static uint64_t last_ns;
static atomic_bool visible;
static atomic_int shown_x, shown_y;

static float limit(float value, float most) { return value < 0 ? 0 : (value > most ? most : value); }

void linuxSdlCursorUpdate(LinuxInputSnapshot *snapshot, unsigned width, unsigned height) {
    bool toggle = (snapshot->buttons >> LEFT_STICK_BUTTON) & 1u;
    snapshot->buttons &= ~(1u << LEFT_STICK_BUTTON);  // the cursor's button is not also a game button
    if (toggle && !toggle_before) {
        active = !active;
        if (active) {
            linuxSdlEventsMouse(&cursor_x, &cursor_y);  // where the mouse last was (the middle of the screen at the start)
            if (cursor_x == 0 && cursor_y == 0) {
                cursor_x = (float)width / 2;
                cursor_y = (float)height / 2;
            }
            last_ns = 0;
        }
        diagnosticsTrace("sdl.cursor=%s", active ? "ON" : "OFF");
    }
    toggle_before = toggle;
    atomic_store(&visible, active);
    if (!active) return;
    if (last_width && (width != last_width || height != last_height)) {  // docked or undocked: the cursor stays at the same place of the screen
        cursor_x = cursor_x * (float)width / (float)last_width;
        cursor_y = cursor_y * (float)height / (float)last_height;
    }
    last_width = width;
    last_height = height;
    float seconds = last_ns && snapshot->timestamp_ns > last_ns ? (float)(snapshot->timestamp_ns - last_ns) * 1e-9f : 0;
    if (seconds > LONGEST_STEP_SECONDS) seconds = LONGEST_STEP_SECONDS;
    last_ns = snapshot->timestamp_ns;
    float stick_x = (float)snapshot->axes[0] / 32767.0f, stick_y = (float)snapshot->axes[1] / 32767.0f;
    float tilt = sqrtf(stick_x * stick_x + stick_y * stick_y);
    if (tilt > DEAD_ZONE) {
        float strength = limit((tilt - DEAD_ZONE) / (1.0f - DEAD_ZONE), 1.0f);
        float distance = strength * strength * SCREENS_PER_SECOND * (float)height * seconds;
        cursor_x += stick_x / tilt * distance;
        cursor_y += stick_y / tilt * distance;
    }
    cursor_x = limit(cursor_x, (float)width - 1);
    cursor_y = limit(cursor_y, (float)height - 1);
    atomic_store(&shown_x, (int)cursor_x);
    atomic_store(&shown_y, (int)cursor_y);
    snapshot->pointer = true;
    snapshot->pointer_x = cursor_x;
    snapshot->pointer_y = cursor_y;
    snapshot->pointer_left = snapshot->axes[5] > TRIGGER_PRESSED;   // ZR
    snapshot->pointer_right = snapshot->axes[4] > TRIGGER_PRESSED;  // ZL
    snapshot->buttons = 0;
    memset(snapshot->axes, 0, sizeof(snapshot->axes));
}

// ---- drawing: the same legacy OpenGL sandbox as the file chooser (state saved and restored around it) --------------------------------
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef double GLdouble;
enum {
    GL_TRIANGLES = 4,
    GL_QUADS = 7,
    GL_TEXTURE_2D = 0x0DE1,
    GL_BLEND = 0x0BE2,
    GL_SRC_ALPHA = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_PROJECTION = 0x1701,
    GL_MODELVIEW = 0x1700,
    GL_TEXTURE = 0x1702,
    GL_TEXTURE0 = 0x84C0,
    GL_CURRENT_PROGRAM = 0x8B8D,
    GL_FRAMEBUFFER = 0x8D40,
    GL_FRAMEBUFFER_BINDING = 0x8CA6,
    GL_DEPTH_TEST = 0x0B71,
    GL_CULL_FACE = 0x0B44,
    GL_SCISSOR_TEST = 0x0C11,
    GL_STENCIL_TEST = 0x0B90,
    GL_LIGHTING = 0x0B50,
    GL_ALPHA_TEST = 0x0BC0,
    GL_FOG = 0x0B60
};
static struct {
    void (*Begin)(GLenum);
    void (*End)(void);
    void (*Vertex2f)(GLfloat, GLfloat);
    void (*Color4f)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*Ortho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
    void (*PushAttrib)(GLbitfield);
    void (*PopAttrib)(void);
    void (*PushClientAttrib)(GLbitfield);
    void (*PopClientAttrib)(void);
    void (*MatrixMode)(GLenum);
    void (*PushMatrix)(void);
    void (*PopMatrix)(void);
    void (*LoadIdentity)(void);
    void (*UseProgram)(GLuint);
    void (*GetIntegerv)(GLenum, GLint *);
    void (*ActiveTexture)(GLenum);
    void (*BlendFunc)(GLenum, GLenum);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
} gl;
static bool gl_ready, draw_failed;

static bool loadFunction(void *slot, const char *name) {
    void *address = (void *)eglGetProcAddress(name);
    if (address) memcpy(slot, &address, sizeof(address));
    return address != NULL;
}
static bool loadGl(void) {
#define LOAD(field)                                                                                                                                            \
    if (!loadFunction(&gl.field, "gl" #field)) return false
    LOAD(Begin);
    LOAD(End);
    LOAD(Vertex2f);
    LOAD(Color4f);
    LOAD(Ortho);
    LOAD(PushAttrib);
    LOAD(PopAttrib);
    LOAD(PushClientAttrib);
    LOAD(PopClientAttrib);
    LOAD(MatrixMode);
    LOAD(PushMatrix);
    LOAD(PopMatrix);
    LOAD(LoadIdentity);
    LOAD(UseProgram);
    LOAD(GetIntegerv);
    LOAD(ActiveTexture);
    LOAD(BlendFunc);
    LOAD(Viewport);
    LOAD(BindFramebuffer);
    LOAD(Enable);
    LOAD(Disable);
#undef LOAD
    return true;
}
// The arrow, in units of a 12 x 19 pointer whose tip is the origin: two triangles for the head and a quadrilateral for the tail.
static void arrow(float x, float y, float unit, float shift_x, float shift_y) {
    static const float head_a[3][2] = {{0, 0}, {0, 16}, {4, 12}}, head_b[3][2] = {{0, 0}, {4, 12}, {11, 11}}, tail[4][2] = {{4, 12}, {7, 19}, {9, 18}, {6, 11}};
    x += shift_x * unit;
    y += shift_y * unit;
    gl.Begin(GL_TRIANGLES);
    for (unsigned i = 0; i < 3; ++i) gl.Vertex2f(x + head_a[i][0] * unit, y + head_a[i][1] * unit);
    for (unsigned i = 0; i < 3; ++i) gl.Vertex2f(x + head_b[i][0] * unit, y + head_b[i][1] * unit);
    gl.End();
    gl.Begin(GL_QUADS);
    for (unsigned i = 0; i < 4; ++i) gl.Vertex2f(x + tail[i][0] * unit, y + tail[i][1] * unit);
    gl.End();
}

void linuxSdlCursorDraw(unsigned width, unsigned height) {
    if (!atomic_load(&visible) || draw_failed || eglGetCurrentContext() == EGL_NO_CONTEXT) return;
    if (!gl_ready) {
        if (!loadGl()) {
            draw_failed = true;
            diagnosticsTrace("sdl.cursor.draw=FAIL (OpenGL functions)");
            return;
        }
        gl_ready = true;
    }
    GLint program = 0, framebuffer = 0;
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &program);
    gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    gl.PushAttrib(0x000FFFFFu);
    gl.PushClientAttrib(0xFFFFFFFFu);
    gl.UseProgram(0);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Viewport(0, 0, (GLsizei)width, (GLsizei)height);
    const GLenum off[] = {GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST, GL_LIGHTING, GL_ALPHA_TEST, GL_FOG, GL_TEXTURE_2D};
    gl.ActiveTexture(GL_TEXTURE0);
    for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); ++i) gl.Disable(off[i]);
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.MatrixMode(GL_TEXTURE);
    gl.PushMatrix();
    gl.LoadIdentity();
    gl.MatrixMode(GL_PROJECTION);
    gl.PushMatrix();
    gl.LoadIdentity();
    gl.Ortho(0, (GLdouble)width, (GLdouble)height, 0, -1, 1);
    gl.MatrixMode(GL_MODELVIEW);
    gl.PushMatrix();
    gl.LoadIdentity();
    float x = (float)atomic_load(&shown_x), y = (float)atomic_load(&shown_y), unit = (float)height / 540.0f;
    static const float outline[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    gl.Color4f(0, 0, 0, 1);
    for (unsigned i = 0; i < 4; ++i) arrow(x, y, unit, outline[i][0], outline[i][1]);
    gl.Color4f(1, 1, 1, 1);
    arrow(x, y, unit, 0, 0);
    gl.MatrixMode(GL_MODELVIEW);
    gl.PopMatrix();
    gl.MatrixMode(GL_PROJECTION);
    gl.PopMatrix();
    gl.MatrixMode(GL_TEXTURE);
    gl.PopMatrix();
    gl.PopClientAttrib();
    gl.PopAttrib();
    gl.UseProgram((GLuint)program);
    gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)framebuffer);
}
