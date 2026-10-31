// SPDX-License-Identifier: BSD-2-Clause
// Pure computation of the TCA8418 KP_GPIO1..3 registers and the driver's health checks (no ESP headers, host-testable).
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t kp_gpio1; // rows 0-7 (ROW0..ROW7)
    uint8_t kp_gpio2; // columns 0-7 (COL0..COL7)
    uint8_t kp_gpio3; // columns 8-9 (COL8, COL9); 0 when cols <= 8
} tca8418_masks_t;

// Rows are clamped to 1..8 and cols to 1..10; 0 counts as the maximum.
tca8418_masks_t tca8418_key_masks(unsigned rows, unsigned cols);

// The readback the keyboard does once a second (#14): C6 EN also resets the TCA8418, which then reads its power-on 0 in
// KP_GPIO1 instead of the row mask. A failed read (the chip held in reset NACKs) is NO_ANSWER, not a verdict.
typedef enum { TCA8418_CHECK_OK, TCA8418_CHECK_RESET, TCA8418_CHECK_NO_ANSWER } tca8418_check_t;
tca8418_check_t tca8418_check_kp_gpio1(bool read_ok, uint8_t value, unsigned rows, unsigned cols);

// Rate limit for the I2C warnings (#14): a chip in reset fails every 20 ms poll, which would flood the RAM log. Logged:
// the first failure of a run, the 100th, 1000th, ... (STILL), and the first success after it (BACK, with the count).
typedef struct {
    uint32_t fails; // failures in the current run
} tca8418_warn_t;
typedef enum { TCA8418_WARN_QUIET, TCA8418_WARN_FIRST, TCA8418_WARN_STILL, TCA8418_WARN_BACK } tca8418_warn_result_t;
tca8418_warn_result_t tca8418_warn_fail(tca8418_warn_t *w);
// BACK puts the run's length in *fails and starts over
tca8418_warn_result_t tca8418_warn_ok(tca8418_warn_t *w, uint32_t *fails);
