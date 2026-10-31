#include "key_matrix.h"

#include "key_codes.h"
#include "tca8418_keymap.h"

#include <stdio.h>
#include <string.h>

#define CODES (KEY_MATRIX_ROWS * KEY_MATRIX_COLS)

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

// Name, key-top label; the label is what fits a 60 px key
typedef struct {
    keyboard_scancode_t scancode;
    char const         *name;
    char const         *label;
} entry_t;

static entry_t const SPECIAL[] = {
    {KEY_SCANCODE_ESCAPE, "ESCAPE", "Esc"},
    {KEY_SCANCODE_SQUARE, "SQUARE", "\xE2\x96\xA0"},        // U+25A0
    {KEY_SCANCODE_TRIANGLE, "TRIANGLE", "\xE2\x96\xB2"},    // U+25B2
    {KEY_SCANCODE_CROSS, "CROSS", "X"},                     // U+2715 is not in the font
    {KEY_SCANCODE_CIRCLE, "CIRCLE", "\xE2\x97\x8F"},        // U+25CF
    {KEY_SCANCODE_CLOUD, "CLOUD", "Cld"},                   // U+2601 is not in the font
    {KEY_SCANCODE_DIAMOND, "DIAMOND", "\xE2\x97\x86"},      // U+25C6
    {KEY_SCANCODE_BACKSPACE, "BACKSPACE", "Bksp"},
    {KEY_SCANCODE_TAB, "TAB", "Tab"},
    {KEY_SCANCODE_FN, "FN", "Fn"},
    {KEY_SCANCODE_LSHIFT, "LSHIFT", "Shft"},
    {KEY_SCANCODE_RSHIFT, "RSHIFT", "Shft"},
    {KEY_SCANCODE_RETURN, "RETURN", "Ret"},
    {KEY_SCANCODE_LCTRL, "LCTRL", "Ctrl"},
    {KEY_SCANCODE_LGUI, "LGUI", "Gui"},
    {KEY_SCANCODE_LALT, "LALT", "Alt"},
    {KEY_SCANCODE_RALT, "RALT", "Alt"},
    {KEY_SCANCODE_SPACE, "SPACE", "Spc"},
    {KEY_SCANCODE_LEFT, "LEFT", "<"},
    {KEY_SCANCODE_RIGHT, "RIGHT", ">"},
    {KEY_SCANCODE_UP, "UP", "^"},
    {KEY_SCANCODE_DOWN, "DOWN", "v"},
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

size_t key_matrix_format(char *out, size_t cap, key_raw_event_t const *ev) {
    int n = snprintf(out, cap, "0x%02x (%u)  %s  %s  Shift %s", ev->code, ev->code, key_matrix_name(ev->code),
                     ev->pressed ? "pressed" : "released", ev->shift ? "on" : "off");
    return n < 0 ? 0 : (size_t)n;
}
