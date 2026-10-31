// SPDX-License-Identifier: BSD-2-Clause
// Pure computation of the TCA8418 KP_GPIO1..3 registers (no ESP headers, host-testable).
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t kp_gpio1; // rows 0-7 (ROW0..ROW7)
    uint8_t kp_gpio2; // columns 0-7 (COL0..COL7)
    uint8_t kp_gpio3; // columns 8-9 (COL8, COL9); 0 when cols <= 8
} tca8418_masks_t;

// Rows are clamped to 1..8 and cols to 1..10; 0 counts as the maximum.
tca8418_masks_t tca8418_key_masks(unsigned rows, unsigned cols);
