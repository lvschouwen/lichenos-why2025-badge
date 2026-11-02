// Host test: the keyboard test's matrix layout and event text (#81)
#include "key_matrix.h"
#include "key_codes.h"
#include "tca8418_keymap.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void label_is(uint8_t code, char const *want) {
    char buf[8];
    if (strcmp(key_matrix_label(code, buf, sizeof(buf)), want) != 0) {
        printf("FAIL label of 0x%02x: %s, want %s\n", code, buf, want);
        failures++;
    }
}

static key_layout_t const *at(uint8_t code) {
    key_layout_t const *keys;
    key_layout(&keys);
    int i = key_layout_index(code);
    return i < 0 ? NULL : &keys[i];
}

// The printed layout (#81): 71 wired keys, rows that add up, the keys where the PCB has them
static void layout_checks(void) {
    key_layout_t const *keys;
    size_t              count = key_layout(&keys);
    CHECK(count == 71);
    int wired = 0;
    for (int code = 1; code <= 0x50; code++) {
        keyboard_scancode_t unknown = tca8418_keymap[code - 1];
        bool                in      = key_layout_index((uint8_t)code) >= 0;
        if (unknown != KEY_SCANCODE_UNKNOWN) {
            wired++;
            CHECK(in);
        }
        if (code >= 0x47 && code <= 0x4f) {
            CHECK(!in);
        }
    }
    CHECK(wired == 71);
    for (size_t i = 0; i < count; i++) {
        CHECK(key_layout_index(keys[i].code) == (int)i);
        CHECK(keys[i].row < KEY_LAYOUT_ROWS && keys[i].w > 0 && keys[i].x + keys[i].w <= KEY_LAYOUT_WIDTH);
        if (i > 0 && keys[i].row == keys[i - 1].row) {
            CHECK(keys[i].x >= keys[i - 1].x + keys[i - 1].w); // sorted, no overlap
        } else if (i > 0) {
            CHECK(keys[i].row == keys[i - 1].row + 1 && keys[i].x == 0); // rows in order, each starts at the left
        }
    }
    int width[KEY_LAYOUT_ROWS] = {0};
    for (size_t i = 0; i < count; i++) {
        width[keys[i].row] += keys[i].w;
    }
    CHECK(width[0] == 48); // the 1u gap between ✕ and ●
    for (int row = 1; row < KEY_LAYOUT_ROWS; row++) {
        CHECK(width[row] == KEY_LAYOUT_WIDTH);
    }
    CHECK(at(0x04)->x + at(0x04)->w == 24 && at(0x05)->x == 28);
    CHECK(at(0x01)->row == 0 && at(0x01)->x == 0);
    CHECK(at(0x45)->row == 2 && at(0x45)->x == 40);
    CHECK(at(0x3c)->row == 1 && at(0x3c)->x == 48);
    CHECK(at(0x37)->x == 44 && at(0x34)->x == 44 && at(0x37)->row == 4 && at(0x34)->row == 5);
    CHECK(at(0x41)->x == 16 && at(0x41)->x + at(0x41)->w == at(0x42)->x);
    CHECK(at(0x42)->x + at(0x42)->w == at(0x43)->x && at(0x43)->x + at(0x43)->w == 36);
    CHECK(key_layout_index(0x47) == -1 && key_layout_index(0x4f) == -1 && key_layout_index(0) == -1);
    CHECK(key_layout_index(0x51) == -1 && key_layout_index(0xff) == -1);
}

int main(void) {
    // code = row * 10 + col + 1, both ways, over the whole matrix
    for (int row = 0; row < KEY_MATRIX_ROWS; row++) {
        for (int col = 0; col < KEY_MATRIX_COLS; col++) {
            uint8_t code = key_matrix_code(row, col);
            int     r, c;
            CHECK(code == row * 10 + col + 1);
            CHECK(key_matrix_pos(code, &r, &c) && r == row && c == col);
        }
    }
    int r, c;
    CHECK(key_matrix_code(8, 0) == 0 && key_matrix_code(0, 10) == 0 && key_matrix_code(-1, 0) == 0);
    CHECK(!key_matrix_pos(0, &r, &c) && !key_matrix_pos(0x51, &r, &c));
    CHECK(key_matrix_pos(0x50, &r, &c) && r == 7 && c == 9); // columns 8-9 are #77's

    CHECK(KEY_MATRIX_ESC_CODE == 1 && strcmp(key_matrix_name(KEY_MATRIX_ESC_CODE), "ESCAPE") == 0);
    CHECK(strcmp(key_matrix_name(0x02), "SQUARE") == 0);
    CHECK(strcmp(key_matrix_name(0x07), "DIAMOND") == 0);
    CHECK(strcmp(key_matrix_name(0x1f), "FN") == 0);
    CHECK(strcmp(key_matrix_name(0x20), "A") == 0);
    CHECK(strcmp(key_matrix_name(0x09), "0") == 0);
    CHECK(strcmp(key_matrix_name(0x0c), "1") == 0);
    CHECK(strcmp(key_matrix_name(0x47), "UNKNOWN") == 0);
    CHECK(strcmp(key_matrix_name(0), "UNKNOWN") == 0);
    CHECK(strcmp(key_matrix_name(0x51), "UNKNOWN") == 0);

    label_is(0x20, "A");
    label_is(0x41, "Spc");
    label_is(0x08, "\xE2\x8C\xAB");   // backspace
    label_is(0x3b, "\xE2\x8F\x8E");   // return
    label_is(0x04, "\xE2\x9C\x95");   // cross
    label_is(0x06, "\xE2\x98\x81");   // cloud
    label_is(0x33, "\xE2\x86\x90");   // left
    label_is(0x29, "\xE2\x87\xA7");   // shift
    label_is(0x38, "\xE2\x87\xA7");
    label_is(0x3e, "\xF0\x9F\xAA\x90"); // the planet
    label_is(0x44, "Alt");              // printed ALT, not AltGr
    label_is(0x3d, "Ctl");
    label_is(0x47, "47"); // unmapped: the hex code, so a surprise key can be identified
    label_is(0x50, "]");

    layout_checks();

    CHECK(key_matrix_color(0x02) == 0xE5484D && key_matrix_color(0x03) == 0xF5821F);
    CHECK(key_matrix_color(0x04) == 0xF2D338 && key_matrix_color(0x05) == 0x3FB950);
    CHECK(key_matrix_color(0x06) == 0x4C9AFF && key_matrix_color(0x07) == 0xA371F7);
    CHECK(key_matrix_color(0x01) == 0 && key_matrix_color(0x20) == 0 && key_matrix_color(0x47) == 0);

    key_raw_event_t ev = {.code = 0x1f, .pressed = true, .shift = false};
    char            text[80];
    key_matrix_format(text, sizeof(text), &ev);
    CHECK(strcmp(text, "0x1f (31)  FN  pressed  Shift off") == 0);
    ev = (key_raw_event_t){.code = 0x29, .pressed = false, .shift = true};
    key_matrix_format(text, sizeof(text), &ev);
    CHECK(strcmp(text, "0x29 (41)  LSHIFT  released  Shift on") == 0);

    // Truncation stays terminated
    char tiny[8];
    key_matrix_format(tiny, sizeof(tiny), &ev);
    CHECK(strlen(tiny) == 7);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("key_matrix_test: ok\n");
    return 0;
}
