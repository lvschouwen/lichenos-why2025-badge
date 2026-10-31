#include "kbd_map.h"

#include "key_codes.h"
#include "tca8418_keymap.h"

static keyboard_scancode_t scancode_of(uint8_t key) {
    // Code 0 means "FIFO empty"; the table starts at code 1
    if (key < 1 || key > sizeof(tca8418_keymap) / sizeof(tca8418_keymap[0])) {
        return KEY_SCANCODE_UNKNOWN;
    }
    return tca8418_keymap[key - 1];
}

bool kbd_map_is_shift(uint8_t key) {
    keyboard_scancode_t s = scancode_of(key);
    return s == KEY_SCANCODE_LSHIFT || s == KEY_SCANCODE_RSHIFT;
}

uint32_t kbd_map_key(uint8_t key, bool shift) {
    keyboard_scancode_t s = scancode_of(key);
    switch (s) {
        case KEY_SCANCODE_UP: return KBD_KEY_UP;
        case KEY_SCANCODE_DOWN: return KBD_KEY_DOWN;
        case KEY_SCANCODE_LEFT: return KBD_KEY_LEFT;
        case KEY_SCANCODE_RIGHT: return KBD_KEY_RIGHT;
        case KEY_SCANCODE_RETURN: return KBD_KEY_ENTER;
        case KEY_SCANCODE_BACKSPACE: return KBD_KEY_BACKSPACE;
        case KEY_SCANCODE_ESCAPE: return KBD_KEY_ESC;
        case KEY_SCANCODE_TAB: return KBD_KEY_NEXT;
        case KEY_SCANCODE_UNKNOWN: return 0;
        default: break;
    }
    char c = keyboard_get_ascii(s, shift ? BADGEVMS_KMOD_LSHIFT : BADGEVMS_KMOD_NONE);
    return (unsigned char)c >= 0x20 && c != 0x7f ? (uint32_t)(unsigned char)c : 0;
}
