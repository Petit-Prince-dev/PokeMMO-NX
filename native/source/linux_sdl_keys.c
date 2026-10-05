#include "linux_sdl_keys.h"
#include <stdatomic.h>
#include <stdio.h>

#define SCANCODE_MASK 0x40000000u  // SDL_SCANCODE_TO_KEYCODE: keys without a character are their scancode with this bit set

uint32_t linuxSdlKeyFromScancode(uint32_t scancode) {
    if (scancode >= 4 && scancode <= 29) return 'a' + (scancode - 4);
    if (scancode >= 30 && scancode <= 38) return '1' + (scancode - 30);
    switch (scancode) {
        case 39: return '0';
        case 40: return '\r';
        case 41: return 27;
        case 42: return '\b';
        case 43: return '\t';
        case 44: return ' ';
        case 45: return '-';
        case 46: return '=';
        case 47: return '[';
        case 48: return ']';
        case 49: return '\\';
        case 51: return ';';
        case 52: return '\'';
        case 53: return '`';
        case 54: return ',';
        case 55: return '.';
        case 56: return '/';
        case 76: return 127;  // delete is a character in SDL 3
        default: break;
    }
    if ((scancode >= 57 && scancode <= 82) || (scancode >= 224 && scancode <= 231)) return scancode | SCANCODE_MASK;
    return 0;
}

const char *linuxSdlKeyName(uint32_t keycode) {
    static char names[8][8];
    static atomic_uint next;
    if (keycode & SCANCODE_MASK) {
        uint32_t scancode = keycode & ~SCANCODE_MASK;
        if (scancode >= 58 && scancode <= 69) {
            char *name = names[atomic_fetch_add(&next, 1) % 8];
            snprintf(name, sizeof(names[0]), "F%u", scancode - 57);
            return name;
        }
        switch (scancode) {
            case 57: return "CapsLock";
            case 70: return "PrintScreen";
            case 71: return "ScrollLock";
            case 72: return "Pause";
            case 73: return "Insert";
            case 74: return "Home";
            case 75: return "PageUp";
            case 77: return "End";
            case 78: return "PageDown";
            case 79: return "Right";
            case 80: return "Left";
            case 81: return "Down";
            case 82: return "Up";
            case 224: return "Left Ctrl";
            case 225: return "Left Shift";
            case 226: return "Left Alt";
            case 227: return "Left GUI";
            case 228: return "Right Ctrl";
            case 229: return "Right Shift";
            case 230: return "Right Alt";
            case 231: return "Right GUI";
            default: return "";
        }
    }
    switch (keycode) {
        case '\r': return "Return";
        case 27: return "Escape";
        case '\b': return "Backspace";
        case '\t': return "Tab";
        case ' ': return "Space";
        case 127: return "Delete";
        default: break;
    }
    if (keycode > 32 && keycode < 127) {
        char *name = names[atomic_fetch_add(&next, 1) % 8];
        name[0] = (char)(keycode >= 'a' && keycode <= 'z' ? keycode - 32 : keycode);
        name[1] = 0;
        return name;
    }
    return "";
}
