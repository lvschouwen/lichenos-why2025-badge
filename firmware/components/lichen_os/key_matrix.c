#include "key_matrix.h"

#include "key_codes.h"
#include "tca8418_keymap.h"

#include <stdio.h>
#include <string.h>

#define CODES (KEY_MATRIX_ROWS * KEY_MATRIX_COLS)

_Static_assert(KEY_LAYOUT_COUNT == 71, "the Carrier keyboard has 71 keys");

static keyboard_scancode_t scancode_of(uint8_t code) {
    return code >= 1 && code <= CODES ? tca8418_keymap[code - 1] : KEY_SCANCODE_UNKNOWN;
}

uint8_t key_matrix_code(int row, int col) {
    if (row < 0 || row >= KEY_MATRIX_ROWS || col < 0 || col >= KEY_MATRIX_COLS) {
        return 0;
    }
    return (uint8_t)(row * KEY_MATRIX_COLS + col + 1);
}

bool key_matrix_pos(uint8_t code, int *row, int *col) {
    if (code < 1 || code > CODES) {
        return false;
    }
    *row = (code - 1) / KEY_MATRIX_COLS;
    *col = (code - 1) % KEY_MATRIX_COLS;
    return true;
}

// From the Carrier PCB's keyboard footprint U14 (hardware repo @797d883), see #81. Esc, the shapes and Backspace are
// 1.5u with a 1u gap in the middle; the space bar is three domes over 5u.
static key_layout_t const LAYOUT[KEY_LAYOUT_COUNT] = {
    {0x01, 0, 0, 6},  {0x02, 0, 6, 6},  {0x03, 0, 12, 6}, {0x04, 0, 18, 6},
    {0x05, 0, 28, 6}, {0x06, 0, 34, 6}, {0x07, 0, 40, 6}, {0x08, 0, 46, 6},
    // ` 1 2 3 4 5 6 7 8 9 0 - =
    {0x0b, 1, 0, 4},  {0x0c, 1, 4, 4},  {0x0d, 1, 8, 4},  {0x0e, 1, 12, 4}, {0x0f, 1, 16, 4}, {0x10, 1, 20, 4},
    {0x11, 1, 24, 4}, {0x12, 1, 28, 4}, {0x13, 1, 32, 4}, {0x14, 1, 36, 4}, {0x09, 1, 40, 4}, {0x0a, 1, 44, 4},
    {0x3c, 1, 48, 4},
    // Tab Q W E R T Y U I O P [ ]
    {0x15, 2, 0, 4},  {0x16, 2, 4, 4},  {0x17, 2, 8, 4},  {0x18, 2, 12, 4}, {0x19, 2, 16, 4}, {0x1a, 2, 20, 4},
    {0x1b, 2, 24, 4}, {0x1c, 2, 28, 4}, {0x1d, 2, 32, 4}, {0x1e, 2, 36, 4}, {0x45, 2, 40, 4}, {0x46, 2, 44, 4},
    {0x50, 2, 48, 4},
    // Fn A S D F G H J K L ; ' Return
    {0x1f, 3, 0, 4},  {0x20, 3, 4, 4},  {0x21, 3, 8, 4},  {0x22, 3, 12, 4}, {0x23, 3, 16, 4}, {0x24, 3, 20, 4},
    {0x25, 3, 24, 4}, {0x26, 3, 28, 4}, {0x27, 3, 32, 4}, {0x28, 3, 36, 4}, {0x39, 3, 40, 4}, {0x3a, 3, 44, 4},
    {0x3b, 3, 48, 4},
    // Shift Z X C V B N M , . / Up Shift
    {0x29, 4, 0, 4},  {0x2a, 4, 4, 4},  {0x2b, 4, 8, 4},  {0x2c, 4, 12, 4}, {0x2d, 4, 16, 4}, {0x2e, 4, 20, 4},
    {0x2f, 4, 24, 4}, {0x30, 4, 28, 4}, {0x31, 4, 32, 4}, {0x32, 4, 36, 4}, {0x36, 4, 40, 4}, {0x37, 4, 44, 4},
    {0x38, 4, 48, 4},
    // Ctrl Gui Alt \, the space bar's three domes, Alt Left Down Right
    {0x3d, 5, 0, 4},  {0x3e, 5, 4, 4},  {0x3f, 5, 8, 4},  {0x40, 5, 12, 4},
    {0x41, 5, 16, 7}, {0x42, 5, 23, 6}, {0x43, 5, 29, 7},
    {0x44, 5, 36, 4}, {0x33, 5, 40, 4}, {0x34, 5, 44, 4}, {0x35, 5, 48, 4},
};

size_t key_layout(key_layout_t const **keys) {
    if (keys) {
        *keys = LAYOUT;
    }
    return KEY_LAYOUT_COUNT;
}

int key_layout_index(uint8_t code) {
    for (int i = 0; i < KEY_LAYOUT_COUNT; i++) {
        if (LAYOUT[i].code == code) {
            return i;
        }
    }
    return -1;
}

// Name, key-top label; the label has to fit a 1u key (about 45 px at 24 px)
typedef struct {
    keyboard_scancode_t scancode;
    char const         *name;
    char const         *label;
} entry_t;

static entry_t const SPECIAL[] = {
    {KEY_SCANCODE_ESCAPE, "ESCAPE", "Esc"},
    {KEY_SCANCODE_SQUARE, "SQUARE", "\xE2\x96\xA0"}, // U+25A0
    {KEY_SCANCODE_TRIANGLE, "TRIANGLE", "\xE2\x96\xB2"}, // U+25B2
    {KEY_SCANCODE_CROSS, "CROSS", "\xE2\x9C\x95"}, // U+2715
    {KEY_SCANCODE_CIRCLE, "CIRCLE", "\xE2\x97\x8F"}, // U+25CF
    {KEY_SCANCODE_CLOUD, "CLOUD", "\xE2\x98\x81"}, // U+2601
    {KEY_SCANCODE_DIAMOND, "DIAMOND", "\xE2\x97\x86"}, // U+25C6
    {KEY_SCANCODE_BACKSPACE, "BACKSPACE", "\xE2\x8C\xAB"},
    {KEY_SCANCODE_TAB, "TAB", "Tab"},
    {KEY_SCANCODE_FN, "FN", "Fn"},
    {KEY_SCANCODE_LSHIFT, "LSHIFT", "\xE2\x87\xA7"},
    {KEY_SCANCODE_RSHIFT, "RSHIFT", "\xE2\x87\xA7"},
    {KEY_SCANCODE_RETURN, "RETURN", "\xE2\x8F\x8E"},
    {KEY_SCANCODE_LCTRL, "LCTRL", "Ctl"},
    {KEY_SCANCODE_LGUI, "LGUI", "\xF0\x9F\xAA\x90"},
    {KEY_SCANCODE_LALT, "LALT", "Alt"},
    {KEY_SCANCODE_RALT, "RALT", "Alt"},
    {KEY_SCANCODE_SPACE, "SPACE", "Spc"},
    {KEY_SCANCODE_LEFT, "LEFT", "\xE2\x86\x90"},
    {KEY_SCANCODE_RIGHT, "RIGHT", "\xE2\x86\x92"},
    {KEY_SCANCODE_UP, "UP", "\xE2\x86\x91"},
    {KEY_SCANCODE_DOWN, "DOWN", "\xE2\x86\x93"},
    {KEY_SCANCODE_COMMA, "COMMA", ","},
    {KEY_SCANCODE_PERIOD, "PERIOD", "."},
    {KEY_SCANCODE_SLASH, "SLASH", "/"},
    {KEY_SCANCODE_SEMICOLON, "SEMICOLON", ";"},
    {KEY_SCANCODE_APOSTROPHE, "APOSTROPHE", "'"},
    {KEY_SCANCODE_EQUALS, "EQUALS", "="},
    {KEY_SCANCODE_MINUS, "MINUS", "-"},
    {KEY_SCANCODE_GRAVE, "GRAVE", "`"},
    {KEY_SCANCODE_BACKSLASH, "BACKSLASH", "\\"},
    {KEY_SCANCODE_LEFTBRACKET, "LEFTBRACKET", "["},
    {KEY_SCANCODE_RIGHTBRACKET, "RIGHTBRACKET", "]"},
};

static entry_t const *special_of(keyboard_scancode_t s) {
    for (size_t i = 0; i < sizeof(SPECIAL) / sizeof(SPECIAL[0]); i++) {
        if (SPECIAL[i].scancode == s) {
            return &SPECIAL[i];
        }
    }
    return NULL;
}

// Letters and digits are one character, taken from the scancode itself
static char char_of(keyboard_scancode_t s) {
    if (s >= KEY_SCANCODE_A && s <= KEY_SCANCODE_Z) {
        return (char)('A' + (s - KEY_SCANCODE_A));
    }
    if (s >= KEY_SCANCODE_1 && s <= KEY_SCANCODE_9) {
        return (char)('1' + (s - KEY_SCANCODE_1));
    }
    return s == KEY_SCANCODE_0 ? '0' : 0;
}

// The legends' colours on the real keycaps
uint32_t key_matrix_color(uint8_t code) {
    switch (scancode_of(code)) {
    case KEY_SCANCODE_SQUARE:
        return 0xE5484D; // red
    case KEY_SCANCODE_TRIANGLE:
        return 0xF5821F; // orange
    case KEY_SCANCODE_CROSS:
        return 0xF2D338; // yellow
    case KEY_SCANCODE_CIRCLE:
        return 0x3FB950; // green
    case KEY_SCANCODE_CLOUD:
        return 0x4C9AFF; // blue
    case KEY_SCANCODE_DIAMOND:
        return 0xA371F7; // purple
    default:
        return 0;
    }
}

key_shape_t key_matrix_shape(uint8_t code) {
    switch (scancode_of(code)) {
    case KEY_SCANCODE_SQUARE: return KEY_SHAPE_SQUARE;
    case KEY_SCANCODE_TRIANGLE: return KEY_SHAPE_TRIANGLE;
    case KEY_SCANCODE_CROSS: return KEY_SHAPE_CROSS;
    case KEY_SCANCODE_CIRCLE: return KEY_SHAPE_CIRCLE;
    case KEY_SCANCODE_CLOUD: return KEY_SHAPE_CLOUD;
    case KEY_SCANCODE_DIAMOND: return KEY_SHAPE_DIAMOND;
    default: return KEY_SHAPE_NONE;
    }
}

char const *key_matrix_label(uint8_t code, char *buf, size_t cap) {
    keyboard_scancode_t s = scancode_of(code);
    entry_t const      *e = special_of(s);
    char                c = char_of(s);
    if (e) {
        snprintf(buf, cap, "%s", e->label);
    } else if (c) {
        snprintf(buf, cap, "%c", c);
    } else {
        snprintf(buf, cap, "%02x", code);
    }
    return buf;
}

char const *key_matrix_name(uint8_t code) {
    static char letter[2];
    keyboard_scancode_t s = scancode_of(code);
    entry_t const      *e = special_of(s);
    if (e) {
        return e->name;
    }
    letter[0] = char_of(s);
    return letter[0] ? letter : "UNKNOWN";
}

size_t key_matrix_format(char *out, size_t cap, lichen_raw_key_t const *ev) {
    int n = snprintf(out, cap, "0x%02x (%u)  %s  %s  Shift %s", ev->code, ev->code, key_matrix_name(ev->code),
                     ev->pressed ? "pressed" : "released", ev->shift ? "on" : "off");
    return n < 0 ? 0 : (size_t)n;
}
