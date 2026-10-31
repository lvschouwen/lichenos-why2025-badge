#pragma once

#include "key_raw.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The badge keyboard as the TCA8418 sees it (#81): an 8 x 10 matrix, code = row * 10 + col + 1 (1-0x50). The labels
// come from board/tca8418_keymap.h. The matrix order is the chip's wiring, not the printed keyboard: the first rows
// (Esc and shapes, digits, QWERTY, home row, bottom letters) look like the real thing, the last ones do not. Pure,
// host-tested in host_tests/key_matrix_test.c.
#define KEY_MATRIX_ROWS 8
#define KEY_MATRIX_COLS 10
#define KEY_MATRIX_ESC_CODE 1 // top left of the matrix

// Code of the key at (row, col), 0 outside the matrix
uint8_t key_matrix_code(int row, int col);
// Row and column of a code; false for 0 or past 0x50
bool key_matrix_pos(uint8_t code, int *row, int *col);
// Up to 4 characters to draw on a key ("Esc", "A", "Spc"); unmapped codes show their hex code
char const *key_matrix_label(uint8_t code, char *buf, size_t cap);
// The BadgeVMS scancode name ("ESCAPE", "A", "SQUARE", "UNKNOWN")
char const *key_matrix_name(uint8_t code);
// "0x1f (31)  FN  pressed  Shift off"
size_t key_matrix_format(char *out, size_t cap, key_raw_event_t const *ev);
