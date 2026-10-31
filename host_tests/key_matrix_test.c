// Host test: the keyboard test's matrix layout and event text (#81)
#include "key_matrix.h"

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
    label_is(0x08, "Bksp");
    label_is(0x47, "47"); // unmapped: the hex code, so a surprise key can be identified
    label_is(0x50, "]");

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
