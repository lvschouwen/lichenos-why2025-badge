// SPDX-License-Identifier: BSD-2-Clause
#include "tca8418_mask.h"

// Low n bits set, n in 0..8
static uint8_t low_bits(unsigned n) { return (uint8_t)((1u << n) - 1u); }

tca8418_masks_t tca8418_key_masks(unsigned rows, unsigned cols) {
    if (rows == 0 || rows > 8) rows = 8;
    if (cols == 0 || cols > 10) cols = 10;
    tca8418_masks_t m;
    m.kp_gpio1 = low_bits(rows);
    m.kp_gpio2 = low_bits(cols > 8 ? 8 : cols);
    m.kp_gpio3 = low_bits(cols > 8 ? cols - 8 : 0);
    return m;
}
