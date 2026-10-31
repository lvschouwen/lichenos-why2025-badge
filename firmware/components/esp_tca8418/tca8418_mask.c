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

tca8418_check_t tca8418_check_kp_gpio1(bool read_ok, uint8_t value, unsigned rows, unsigned cols) {
    if (!read_ok) return TCA8418_CHECK_NO_ANSWER;
    return value == tca8418_key_masks(rows, cols).kp_gpio1 ? TCA8418_CHECK_OK : TCA8418_CHECK_RESET;
}

tca8418_warn_result_t tca8418_warn_fail(tca8418_warn_t *w) {
    if (w->fails < UINT32_MAX) w->fails++;
    if (w->fails == 1) return TCA8418_WARN_FIRST;
    uint32_t n = w->fails;
    if (n < 100) return TCA8418_WARN_QUIET;
    while (n % 10 == 0) n /= 10;
    return n == 1 ? TCA8418_WARN_STILL : TCA8418_WARN_QUIET;
}

tca8418_warn_result_t tca8418_warn_ok(tca8418_warn_t *w, uint32_t *fails) {
    if (w->fails == 0) return TCA8418_WARN_QUIET;
    *fails   = w->fails;
    w->fails = 0;
    return TCA8418_WARN_BACK;
}
