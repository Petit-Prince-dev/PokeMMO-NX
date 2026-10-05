#include "linux_file_picker.h"
#include "linux_directories.h"
#include "linux_files.h"
#include "linux_sdl.h"
#include <EGL/egl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

extern const unsigned char default_font_bin[];  // libnx's console font: 256 glyphs of 16x16 pixels, a row is a 16-bit word, bit 15 is the left pixel

#define MAX_ENTRIES 512u
#define PATH_BYTES 512u
#define NAME_BYTES 96u
#define VIEW_WIDTH 1280.0f
#define VIEW_HEIGHT 720.0f
#define SCALE 1.5f  // the font is an 8x8 font drawn twice as large: 1.5 makes every font pixel an exact 3x3 block
#define GLYPH (16.0f * SCALE)
#define ADVANCE (14.0f * SCALE)
#define PANEL_X 60.0f
#define PANEL_Y 30.0f
#define PANEL_W 1160.0f
#define PANEL_H 660.0f
#define TEXT_X (PANEL_X + 28.0f)
#define LIST_TOP 132.0f
#define ROW_HEIGHT 30.0f
#define LIST_ROWS 16u
#define BUTTON_Y 628.0f
#define BUTTON_H 46.0f
#define CANCEL_X 1000.0f
#define CANCEL_W 190.0f
#define CHOOSE_X 740.0f
#define CHOOSE_W 240.0f
#define COMMAND_RING 64u

typedef struct {
    char name[NAME_BYTES];
    bool directory;
} Entry;
typedef enum { CMD_UP, CMD_DOWN, CMD_PAGE_UP, CMD_PAGE_DOWN, CMD_ACCEPT, CMD_BACK, CMD_CANCEL, CMD_HERE, CMD_FILTER, CMD_TAP, CMD_SCROLL } CommandType;
typedef struct {
    CommandType type;
    float x, y;
    int rows;
} Command;

static struct {
    int action;
    char title[NAME_BYTES], folder[PATH_BYTES], name[128];
    unsigned filter_count, filter, pattern_counts[LINUX_PICKER_FILTERS];
    char filter_names[LINUX_PICKER_FILTERS][48], patterns[LINUX_PICKER_FILTERS][LINUX_PICKER_PATTERNS][24];
    Entry entries[MAX_ENTRIES];
    unsigned count, selected, first;
    bool listing_failed, truncated, accepted, finished, opaque;
    char result[PATH_BYTES];
} picker;
static atomic_bool active, hold, running, draw_failed, fresh_input;
static atomic_flag guard = ATOMIC_FLAG_INIT, feed_guard = ATOMIC_FLAG_INIT;
static Command ring[COMMAND_RING];
static atomic_uint ring_head, ring_tail;
static void lock(void) {
    while (atomic_flag_test_and_set_explicit(&guard, memory_order_acquire)) svcSleepThread(20000);
}
static void unlock(void) { atomic_flag_clear_explicit(&guard, memory_order_release); }

// ---- the folder being shown ------------------------------------------------------------------------------------------------------
static bool folderOnly(void) { return picker.action == LINUX_PICKER_FOLDER || picker.action == LINUX_PICKER_CREATE_FOLDER; }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static bool globMatch(const char *pattern, const char *text) {  // * and ? as in a shell, letters without regard to case
    const char *star = NULL, *mark = NULL;
    while (*text) {
        if (*pattern == '*') {
            star = pattern++;
            mark = text;
        } else if (*pattern == '?' || lower(*pattern) == lower(*text)) {
            ++pattern;
            ++text;
        } else if (star) {
            pattern = star + 1;
            text = ++mark;
        } else
            return false;
    }
    while (*pattern == '*') ++pattern;
    return !*pattern;
}
static bool matchesFilter(const char *name) {  // lock held
    if (!picker.filter_count || !picker.pattern_counts[picker.filter]) return true;
    for (unsigned p = 0; p < picker.pattern_counts[picker.filter]; ++p) {
        if (globMatch(picker.patterns[picker.filter][p], name)) return true;
    }
    return false;
}
static void joinPath(char *out, size_t size, const char *folder, const char *name) {  // empty when it does not fit
    size_t folder_length = strlen(folder), name_length = strlen(name), at = folder_length;
    bool slash = strcmp(folder, "/") != 0;
    if (folder_length + (slash ? 1u : 0u) + name_length + 1 > size) {
        out[0] = 0;
        return;
    }
    memcpy(out, folder, folder_length);
    if (slash) out[at++] = '/';
    memcpy(out + at, name, name_length + 1);
}
static void addEntry(const char *name, bool directory) {
    if (picker.count >= MAX_ENTRIES) {
        picker.truncated = true;
        return;
    }
    snprintf(picker.entries[picker.count].name, NAME_BYTES, "%s", name);
    picker.entries[picker.count++].directory = directory;
}
static int compareEntries(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->directory != y->directory) return x->directory ? -1 : 1;
    const char *p = x->name, *q = y->name;
    for (; *p && *q && lower(*p) == lower(*q); ++p, ++q) {}
    return (unsigned char)lower(*p) - (unsigned char)lower(*q);
}
static void listFolder(void) {  // lock held
    picker.count = picker.selected = picker.first = 0;
    picker.listing_failed = picker.truncated = false;
    bool at_root = !strcmp(picker.folder, "/");
    if (!at_root) addEntry("..", true);
    unsigned sortable = picker.count;
    void *directory = linuxAbiOpendir(picker.folder);
    if (!directory)
        picker.listing_failed = true;
    else {
        for (LinuxDirectoryEntry *entry; (entry = linuxAbiReaddir64(directory)) != NULL;) {
            const char *name = entry->name;
            if (!name[0] || name[0] == '.' || strlen(name) >= NAME_BYTES) continue;
            bool is_directory = entry->type == 4;
            if (entry->type != 4 && entry->type != 8) {
                char full[PATH_BYTES];
                LinuxFileStat status;
                joinPath(full, sizeof(full), picker.folder, name);
                if (linuxAbiXstat(0, full, &status) == 0) is_directory = (status.mode & 0170000) == 0040000;
            }
            if (!is_directory && (folderOnly() || !matchesFilter(name))) continue;
            addEntry(name, is_directory);
        }
        linuxAbiClosedir(directory);
    }
    if (at_root) {  // the SD card is a mount, not a folder of the game's tree
        bool listed = false;
        for (unsigned i = 0; i < picker.count; ++i)
            if (!strcmp(picker.entries[i].name, "sd")) listed = true;
        if (!listed) addEntry("sd", true);
    }
    qsort(picker.entries + sortable, picker.count - sortable, sizeof(Entry), compareEntries);
}
static void keepSelectionVisible(void) {
    if (picker.selected < picker.first) picker.first = picker.selected;
    if (picker.selected >= picker.first + LIST_ROWS) picker.first = picker.selected + 1 - LIST_ROWS;
}
static void enterFolder(const char *name) {
    char next[PATH_BYTES];
    joinPath(next, sizeof(next), picker.folder, name);
    if (!next[0]) return;
    snprintf(picker.folder, sizeof(picker.folder), "%s", next);
    listFolder();
}
static void parentFolder(void) {
    if (!strcmp(picker.folder, "/")) return;
    char came_from[NAME_BYTES];
    const char *slash = strrchr(picker.folder, '/');
    snprintf(came_from, sizeof(came_from), "%s", slash ? slash + 1 : "");
    if (slash && slash != picker.folder)
        picker.folder[slash - picker.folder] = 0;
    else
        strcpy(picker.folder, "/");
    listFolder();
    for (unsigned i = 0; i < picker.count; ++i)
        if (!strcmp(picker.entries[i].name, came_from)) {
            picker.selected = i;
            break;
        }
    keepSelectionVisible();
}
static bool folderExists(const char *path) {
    void *directory = linuxAbiOpendir(path);
    if (directory) linuxAbiClosedir(directory);
    return directory != NULL;
}

// ---- what the player asks for ----------------------------------------------------------------------------------------------------
static void activate(unsigned index) {  // lock held
    if (index >= picker.count) return;
    const Entry *entry = &picker.entries[index];
    if (entry->directory) {
        if (!strcmp(entry->name, ".."))
            parentFolder();
        else
            enterFolder(entry->name);
        return;
    }
    if (folderOnly()) return;
    joinPath(picker.result, sizeof(picker.result), picker.folder, entry->name);
    if (picker.result[0]) picker.accepted = picker.finished = true;
}
static void chooseHere(void) {  // lock held
    if (picker.action == LINUX_PICKER_OPEN) return;
    if (folderOnly())
        snprintf(picker.result, sizeof(picker.result), "%s", picker.folder);
    else
        joinPath(picker.result, sizeof(picker.result), picker.folder, picker.name[0] ? picker.name : "file");
    if (picker.result[0]) picker.accepted = picker.finished = true;
}
static void apply(const Command *command) {  // lock held
    switch (command->type) {
        case CMD_UP:
            if (picker.selected) --picker.selected;
            break;
        case CMD_DOWN:
            if (picker.selected + 1 < picker.count) ++picker.selected;
            break;
        case CMD_PAGE_UP: picker.selected = picker.selected > LIST_ROWS ? picker.selected - LIST_ROWS : 0; break;
        case CMD_PAGE_DOWN:
            picker.selected = picker.selected + LIST_ROWS < picker.count ? picker.selected + LIST_ROWS : (picker.count ? picker.count - 1 : 0);
            break;
        case CMD_ACCEPT: activate(picker.selected); return;
        case CMD_BACK: parentFolder(); return;
        case CMD_CANCEL: picker.finished = true; return;
        case CMD_HERE: chooseHere(); return;
        case CMD_FILTER:
            if (picker.filter_count > 1) {
                picker.filter = (picker.filter + 1) % picker.filter_count;
                listFolder();
            }
            return;
        case CMD_SCROLL: {
            int first = (int)picker.first + command->rows, last = (int)picker.count - (int)LIST_ROWS;
            if (last < 0) last = 0;
            picker.first = (unsigned)(first < 0 ? 0 : (first > last ? last : first));
            return;
        }
        case CMD_TAP: {
            if (command->y >= BUTTON_Y && command->y <= BUTTON_Y + BUTTON_H) {
                if (command->x >= CANCEL_X && command->x <= CANCEL_X + CANCEL_W)
                    picker.finished = true;
                else if (picker.action != LINUX_PICKER_OPEN && command->x >= CHOOSE_X && command->x <= CHOOSE_X + CHOOSE_W)
                    chooseHere();
            } else if (command->y >= LIST_TOP && command->y < LIST_TOP + LIST_ROWS * ROW_HEIGHT) {
                unsigned index = picker.first + (unsigned)((command->y - LIST_TOP) / ROW_HEIGHT);
                if (index < picker.count) {
                    picker.selected = index;
                    activate(index);
                }
            }
            return;
        }
    }
    keepSelectionVisible();
}

// ---- input: edges of the sampled controller and touch screen become commands ---------------------------------------------------------
static void push(CommandType type, float x, float y, int rows) {
    unsigned tail = atomic_load(&ring_tail);
    if (tail - atomic_load(&ring_head) >= COMMAND_RING) return;
    ring[tail % COMMAND_RING] = (Command){type, x, y, rows};
    atomic_store(&ring_tail, tail + 1);
}
static bool pop(Command *command) {
    unsigned head = atomic_load(&ring_head);
    if (head == atomic_load(&ring_tail)) return false;
    *command = ring[head % COMMAND_RING];
    atomic_store(&ring_head, head + 1);
    return true;
}
void linuxFilePickerFeed(const LinuxInputSnapshot *s) {
    static LinuxInputSnapshot before;
    static bool have_before, touching, dragged;
    static float start_x, start_y, last_y;
    static int direction;
    static uint64_t repeat_ns;
    while (atomic_flag_test_and_set_explicit(&feed_guard, memory_order_acquire)) {}
    if (!atomic_load(&active)) {
        // The chooser is closed: the buttons that closed it must be let go before the game hears of any input again.
        bool calm =
            !s->fingers && (!s->gamepad || ((s->buttons & 0x7fffu) == 0 && s->axes[0] > -9000 && s->axes[0] < 9000 && s->axes[1] > -9000 && s->axes[1] < 9000));
        if (calm) atomic_store(&hold, false);
        have_before = false;
        atomic_flag_clear_explicit(&feed_guard, memory_order_release);
        return;
    }
    if (atomic_exchange(&fresh_input, false)) have_before = false;
    if (!have_before) {  // what is held when the chooser opens does not count
        before = *s;
        have_before = true;
        touching = dragged = false;
        direction = 0;
        repeat_ns = 0;
        atomic_flag_clear_explicit(&feed_guard, memory_order_release);
        return;
    }
    if (s->gamepad) {
        uint32_t pressed = s->buttons & ~before.buttons;
        if (pressed & (1u << 0)) push(CMD_ACCEPT, 0, 0, 0);      // A
        if (pressed & (1u << 1)) push(CMD_BACK, 0, 0, 0);        // B
        if (pressed & (1u << 2)) push(CMD_CANCEL, 0, 0, 0);      // X
        if (pressed & (1u << 3)) push(CMD_HERE, 0, 0, 0);        // Y
        if (pressed & (1u << 4)) push(CMD_FILTER, 0, 0, 0);      // minus
        if (pressed & (1u << 9)) push(CMD_PAGE_UP, 0, 0, 0);     // L
        if (pressed & (1u << 10)) push(CMD_PAGE_DOWN, 0, 0, 0);  // R
        int now_direction = ((s->buttons >> 11) & 1u) || s->axes[1] < -16000 ? -1 : (((s->buttons >> 12) & 1u) || s->axes[1] > 16000 ? 1 : 0);
        if (now_direction != direction) {
            direction = now_direction;
            if (direction) {
                push(direction < 0 ? CMD_UP : CMD_DOWN, 0, 0, 0);
                repeat_ns = s->timestamp_ns + 400000000ull;
            }
        } else if (direction && s->timestamp_ns >= repeat_ns) {
            push(direction < 0 ? CMD_UP : CMD_DOWN, 0, 0, 0);
            repeat_ns = s->timestamp_ns + 90000000ull;
        }
    }
    if (s->fingers && !before.fingers) {
        touching = true;
        dragged = false;
        start_x = s->x;
        start_y = last_y = s->y;
    } else if (s->fingers && touching) {
        if (!dragged && (s->y - start_y > 24.0f || start_y - s->y > 24.0f)) dragged = true;
        if (dragged) {
            int rows = (int)((last_y - s->y) / ROW_HEIGHT);
            if (rows) {
                push(CMD_SCROLL, 0, 0, rows);
                last_y -= (float)rows * ROW_HEIGHT;
            }
        }
    } else if (!s->fingers && before.fingers && touching) {
        if (!dragged) push(CMD_TAP, start_x, start_y, 0);
        touching = false;
    }
    before = *s;
    atomic_flag_clear_explicit(&feed_guard, memory_order_release);
}
bool linuxFilePickerOpen(void) { return atomic_load(&active); }
bool linuxFilePickerCapturesInput(void) { return atomic_load(&active) || atomic_load(&hold); }
void linuxFilePickerReset(void) {
    atomic_store(&active, false);
    atomic_store(&hold, false);
    atomic_store(&draw_failed, false);
    atomic_store(&ring_head, 0);
    atomic_store(&ring_tail, 0);
}

// ---- drawing: legacy OpenGL on top of whatever the game drew, state saved and restored around it ------------------------------------
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef double GLdouble;
enum {
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
    GL_RGBA = 0x1908,
    GL_UNSIGNED_BYTE = 0x1401,
    GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_WRAP_S = 0x2802,
    GL_TEXTURE_WRAP_T = 0x2803,
    GL_NEAREST = 0x2600,
    GL_CLAMP_TO_EDGE = 0x812F,
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
    void (*TexCoord2f)(GLfloat, GLfloat);
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
    void (*BindTexture)(GLenum, GLuint);
    void (*GenTextures)(GLsizei, GLuint *);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*BlendFunc)(GLenum, GLenum);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
} gl;
static bool gl_ready;
static GLuint font_texture;
static uint32_t atlas[256 * 256];

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
    LOAD(TexCoord2f);
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
    LOAD(BindTexture);
    LOAD(GenTextures);
    LOAD(TexImage2D);
    LOAD(TexParameteri);
    LOAD(BlendFunc);
    LOAD(Viewport);
    LOAD(BindFramebuffer);
    LOAD(Enable);
    LOAD(Disable);
#undef LOAD
    return true;
}
// Called with the game's OpenGL state saved: binds a new texture on the active unit.
static void createFontTexture(void) {
    for (unsigned code = 0; code < 256; ++code)
        for (unsigned y = 0; y < 16; ++y) {
            unsigned row = default_font_bin[code * 32 + y * 2] | ((unsigned)default_font_bin[code * 32 + y * 2 + 1] << 8);
            for (unsigned x = 0; x < 16; ++x) atlas[((code / 16) * 16 + y) * 256 + (code % 16) * 16 + x] = (row >> (15 - x)) & 1u ? 0xFFFFFFFFu : 0x00FFFFFFu;
        }
    gl.GenTextures(1, &font_texture);
    gl.BindTexture(GL_TEXTURE_2D, font_texture);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas);
}
static void rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    gl.Disable(GL_TEXTURE_2D);
    gl.Color4f(r, g, b, a);
    gl.Begin(GL_QUADS);
    gl.Vertex2f(x, y);
    gl.Vertex2f(x + w, y);
    gl.Vertex2f(x + w, y + h);
    gl.Vertex2f(x, y + h);
    gl.End();
}
static void text(float x, float y, const char *string, unsigned max_chars, float r, float g, float b) {
    gl.Enable(GL_TEXTURE_2D);
    gl.Color4f(r, g, b, 1.0f);
    gl.Begin(GL_QUADS);
    x -= 4.0f * SCALE;  // the glyphs leave the first four pixels of their cell empty
    for (unsigned i = 0; string[i] && i < max_chars; ++i, x += ADVANCE) {
        unsigned code = (unsigned char)string[i];
        if (code == ' ') continue;
        float u = (float)(code % 16) / 16.0f, v = (float)(code / 16) / 16.0f, d = 1.0f / 16.0f;
        gl.TexCoord2f(u, v);
        gl.Vertex2f(x, y);
        gl.TexCoord2f(u + d, v);
        gl.Vertex2f(x + GLYPH, y);
        gl.TexCoord2f(u + d, v + d);
        gl.Vertex2f(x + GLYPH, y + GLYPH);
        gl.TexCoord2f(u, v + d);
        gl.Vertex2f(x, y + GLYPH);
    }
    gl.End();
}
static void drawPanel(void) {  // lock held
    const unsigned columns = (unsigned)((PANEL_W - 56.0f) / ADVANCE);
    rect(0, 0, VIEW_WIDTH, VIEW_HEIGHT, 0.0f, 0.0f, 0.0f, picker.opaque ? 1.0f : 0.72f);
    rect(PANEL_X - 3, PANEL_Y - 3, PANEL_W + 6, PANEL_H + 6, 0.55f, 0.55f, 0.55f, 1.0f);
    rect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 0.07f, 0.07f, 0.07f, 1.0f);
    char line[PATH_BYTES + 64];
    text(TEXT_X, 46, picker.title[0] ? picker.title : "Choose", columns, 1.0f, 1.0f, 1.0f);
    if (picker.filter_count) {
        snprintf(line, sizeof(line), "Filter: %s", picker.filter_names[picker.filter]);
        text(PANEL_X + PANEL_W - 28.0f - ADVANCE * (float)strlen(line), 46, line, columns, 0.7f, 0.7f, 0.7f);
    }
    size_t length = strlen(picker.folder);
    snprintf(line, sizeof(line), "%s%s", length > columns ? "..." : "", length > columns ? picker.folder + length - (columns - 3) : picker.folder);
    text(TEXT_X, 86, line, columns, 0.7f, 0.7f, 0.7f);
    rect(PANEL_X + 20, 120, PANEL_W - 40, 2, 0.35f, 0.35f, 0.35f, 1.0f);
    if (picker.listing_failed)
        text(TEXT_X, LIST_TOP + 4, "This folder cannot be read (B: back)", columns, 0.9f, 0.9f, 0.9f);
    else if (!picker.count)
        text(TEXT_X, LIST_TOP + 4, "(empty)", columns, 0.7f, 0.7f, 0.7f);
    for (unsigned row = 0; row < LIST_ROWS && picker.first + row < picker.count; ++row) {
        unsigned index = picker.first + row;
        const Entry *entry = &picker.entries[index];
        float y = LIST_TOP + (float)row * ROW_HEIGHT;
        if (index == picker.selected) rect(PANEL_X + 14, y - 2, PANEL_W - 28, ROW_HEIGHT - 2, 0.3f, 0.3f, 0.3f, 1.0f);
        snprintf(line, sizeof(line), "%s%s", entry->name, entry->directory && strcmp(entry->name, "..") ? "/" : "");
        if (strlen(line) > columns) {
            line[columns - 3] = '.';
            line[columns - 2] = '.';
            line[columns - 1] = '.';
            line[columns] = 0;
        }
        if (entry->directory)
            text(TEXT_X, y + 1, line, columns, 1.0f, 1.0f, 1.0f);
        else
            text(TEXT_X, y + 1, line, columns, 0.75f, 0.75f, 0.75f);
    }
    if (picker.count > LIST_ROWS) {
        snprintf(line, sizeof(line), "%u/%u", picker.selected + 1, picker.count);
        text(PANEL_X + PANEL_W - 28.0f - ADVANCE * (float)strlen(line), 86, line, columns, 0.7f, 0.7f, 0.7f);
    }
    rect(PANEL_X + 20, 614, PANEL_W - 40, 2, 0.35f, 0.35f, 0.35f, 1.0f);
    text(TEXT_X, 626, "A open/pick  B back  X cancel", 31, 0.65f, 0.65f, 0.65f);
    snprintf(line, sizeof(line), "%s%sL/R page", picker.action != LINUX_PICKER_OPEN ? "Y folder  " : "", picker.filter_count > 1 ? "- filter  " : "");
    text(TEXT_X, 654, line, 31, 0.65f, 0.65f, 0.65f);
    rect(CANCEL_X, BUTTON_Y, CANCEL_W, BUTTON_H, 0.22f, 0.22f, 0.22f, 1.0f);
    text(CANCEL_X + (CANCEL_W - 6 * ADVANCE) / 2, BUTTON_Y + 11, "Cancel", 6, 1.0f, 1.0f, 1.0f);
    if (picker.action != LINUX_PICKER_OPEN) {
        rect(CHOOSE_X, BUTTON_Y, CHOOSE_W, BUTTON_H, 0.38f, 0.38f, 0.38f, 1.0f);
        text(CHOOSE_X + (CHOOSE_W - 15 * ADVANCE) / 2, BUTTON_Y + 11, "Use this folder", 15, 1.0f, 1.0f, 1.0f);
    }
}
void linuxFilePickerDraw(unsigned width, unsigned height) {
    if (!atomic_load(&active) || atomic_load(&draw_failed)) return;
    GLint program = 0, framebuffer = 0;
    if (!gl_ready) {
        if (!loadGl()) {
            atomic_store(&draw_failed, true);
            return;
        }
        gl_ready = true;
    }
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &program);
    gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    gl.PushAttrib(0x000FFFFFu);
    gl.PushClientAttrib(0xFFFFFFFFu);
    gl.UseProgram(0);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Viewport(0, 0, (GLsizei)width, (GLsizei)height);
    const GLenum off[] = {GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST, GL_LIGHTING, GL_ALPHA_TEST, GL_FOG};
    for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); ++i) gl.Disable(off[i]);
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.MatrixMode(GL_TEXTURE);
    gl.PushMatrix();
    gl.LoadIdentity();
    gl.MatrixMode(GL_PROJECTION);
    gl.PushMatrix();
    gl.LoadIdentity();
    gl.Ortho(0, VIEW_WIDTH, VIEW_HEIGHT, 0, -1, 1);
    gl.MatrixMode(GL_MODELVIEW);
    gl.PushMatrix();
    gl.LoadIdentity();
    if (!font_texture)
        createFontTexture();
    else
        gl.BindTexture(GL_TEXTURE_2D, font_texture);
    lock();
    drawPanel();
    unlock();
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

// ---- the dialog itself ------------------------------------------------------------------------------------------------------------
bool linuxFilePickerRun(const LinuxPickerRequest *request, char *result, size_t result_size) {
    if (!request || !result || !result_size || atomic_exchange(&running, true)) return false;
    result[0] = 0;
    atomic_store(&draw_failed, false);
    lock();
    memset(&picker, 0, sizeof(picker));
    picker.action = request->action;
    snprintf(picker.title, sizeof(picker.title), "%s", request->title ? request->title : "");
    snprintf(picker.name, sizeof(picker.name), "%s", request->name ? request->name : "");
    picker.filter_count = request->filter_count < LINUX_PICKER_FILTERS ? request->filter_count : LINUX_PICKER_FILTERS;
    for (unsigned f = 0; f < picker.filter_count; ++f) {
        snprintf(picker.filter_names[f], sizeof(picker.filter_names[f]), "%s", request->filter_names[f] ? request->filter_names[f] : "");
        picker.pattern_counts[f] = request->pattern_counts[f] < LINUX_PICKER_PATTERNS ? request->pattern_counts[f] : LINUX_PICKER_PATTERNS;
        for (unsigned p = 0; p < picker.pattern_counts[f]; ++p)
            snprintf(picker.patterns[f][p], sizeof(picker.patterns[f][p]), "%s", request->patterns[f][p] ? request->patterns[f][p] : "");
    }
    // Start where the game asked, else in its ROM folder, else on the SD card.
    static const char *const fallbacks[] = {"/game/roms", "/sd", "/"};
    const char *start = request->folder && request->folder[0] == '/' && folderExists(request->folder) ? request->folder : NULL;
    for (unsigned i = 0; !start && i < sizeof(fallbacks) / sizeof(fallbacks[0]); ++i)
        if (folderExists(fallbacks[i])) start = fallbacks[i];
    snprintf(picker.folder, sizeof(picker.folder), "%s", start ? start : "/");
    size_t length = strlen(picker.folder);
    while (length > 1 && picker.folder[length - 1] == '/') picker.folder[--length] = 0;
    picker.opaque = linuxSdlThreadHasContext();  // called by the thread that renders: the game draws nothing meanwhile, so the chooser must
    listFolder();
    unlock();
    atomic_store(&ring_head, 0);
    atomic_store(&ring_tail, 0);
    atomic_store(&fresh_input, true);
    atomic_store(&active, true);
    bool give_up = false;
    for (;;) {
        if (picker.opaque) {
            if (!linuxSdlPresentFrame()) give_up = true;
        } else
            svcSleepThread(8000000ull);
        if (atomic_load(&draw_failed)) give_up = true;
        lock();
        for (Command command; !picker.finished && pop(&command);) apply(&command);
        bool finished = picker.finished || give_up;
        unlock();
        if (finished) break;
    }
    atomic_store(&active, false);
    atomic_store(&hold, true);
    lock();
    bool accepted = picker.accepted && !give_up;
    if (accepted) snprintf(result, result_size, "%s", picker.result);
    unlock();
    atomic_store(&running, false);
    return accepted;
}
