#include "client_update_internal.h"
#include "display.h"
#include <string.h>
#include <switch.h>

extern const unsigned char default_font_bin[];
static Framebuffer screen;
static bool opened;
enum { WIDTH = 1280, HEIGHT = 720, ADVANCE = 14 };

bool updateScreenOpen(void) {
    if (opened) return true;
    if (!displayPrepareWindow() || R_FAILED(nwindowSetDimensions(nwindowGetDefault(), WIDTH, HEIGHT))) return false;
    if (R_FAILED(framebufferCreate(&screen, nwindowGetDefault(), WIDTH, HEIGHT, PIXEL_FORMAT_RGBA_8888, 3))) return false;
    if (R_FAILED(framebufferMakeLinear(&screen))) {
        framebufferClose(&screen);
        return false;
    }
    opened = true;
    return true;
}
static void centered(uint32_t *pixels, unsigned stride, unsigned y, const char *text) {
    size_t length = strlen(text);
    if (length > WIDTH / ADVANCE) length = WIDTH / ADVANCE;
    unsigned left = (WIDTH - (unsigned)length * ADVANCE) / 2;
    for (unsigned c = 0; c < length; ++c)
        for (unsigned row = 0; row < 16; ++row) {
            const unsigned char *glyph = default_font_bin + (unsigned char)text[c] * 32 + row * 2;
            unsigned bits = glyph[0] | ((unsigned)glyph[1] << 8);
            for (unsigned x = 0; x < ADVANCE; ++x)
                if (bits & (1u << (15 - (x + 2)))) pixels[(y + row) * stride + left + c * ADVANCE + x] = 0xffffffff;
        }
}
void updateScreenDraw(unsigned progress, const char *message, const char *detail) {
    if (!opened) return;
    u32 pitch = 0;
    uint32_t *pixels = framebufferBegin(&screen, &pitch);
    if (!pixels) return;
    unsigned stride = pitch / sizeof(*pixels);
    for (unsigned i = 0; i < stride * HEIGHT; ++i) pixels[i] = 0xff000000;
    centered(pixels, stride, 326, message);
    if (detail)
        centered(pixels, stride, 384, detail);
    else {
        unsigned filled = 640 * (progress > 1000 ? 1000 : progress) / 1000;
        for (unsigned y = 374; y < 382; ++y)
            for (unsigned x = 0; x < 640; ++x) pixels[y * stride + 320 + x] = x < filled ? 0xffffffff : 0xff303030;
    }
    framebufferEnd(&screen);
}
void updateScreenClose(void) {
    if (opened) framebufferClose(&screen);
    opened = false;
}
