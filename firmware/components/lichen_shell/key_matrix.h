#pragma once

#include "key_raw.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The badge keyboard as the TCA8418 sees it (#81): an 8 x 10 matrix, code = row * 10 + col + 1 (1-0x50), labels from
// board/tca8418_keymap.h. Codes 0x47-0x4f are not wired. The drawing follows the printed keyboard instead of the matrix
// order: key_layout() holds the 71 keys with their printed row and width, taken from the Carrier PCB's keyboard
// footprint U14 (hardware repo @797d883; its ROW_5/ROW_6 net names are swapped against the chip's codes). Pure,
// host-tested in host_tests/key_matrix_test.c.
#define KEY_MATRIX_ROWS 8
#define KEY_MATRIX_COLS 10
#define KEY_MATRIX_ESC_CODE 1 // top left of the matrix

// Code of the key at (row, col), 0 outside the matrix
uint8_t key_matrix_code(int row, int col);
// Row and column of a code; false for 0 or past 0x50
bool key_matrix_pos(uint8_t code, int *row, int *col);

#define KEY_LAYOUT_COUNT 71
#define KEY_LAYOUT_ROWS  6
#define KEY_LAYOUT_WIDTH 52 // quarter keys: the 13 keys of the digit row

typedef struct {
    uint8_t code; // TCA8418 code
    uint8_t row;  // printed row, 0 = Esc and the shape keys
    uint8_t x, w; // left edge and width in quarter keys
} key_layout_t;

// The wired keys, row by row, left to right; returns KEY_LAYOUT_COUNT
size_t key_layout(key_layout_t const **keys);
// Index of a code in key_layout(), -1 for unwired codes (0, 0x47-0x4f, past 0x50)
int key_layout_index(uint8_t code);
// Printed colour (0xRRGGBB) of the six shape keys, 0 for every other key
uint32_t key_matrix_color(uint8_t code);
// Up to 4 characters to draw on a key ("Esc", "A", "Spc"); unmapped codes show their hex code
char const *key_matrix_label(uint8_t code, char *buf, size_t cap);
// The BadgeVMS scancode name ("ESCAPE", "A", "SQUARE", "UNKNOWN")
char const *key_matrix_name(uint8_t code);
// "0x1f (31)  FN  pressed  Shift off"
size_t key_matrix_format(char *out, size_t cap, key_raw_event_t const *ev);
