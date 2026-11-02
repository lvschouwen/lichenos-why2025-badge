#pragma once

#include <stdbool.h>
#include <stdint.h>

// R16 (#12): on stock badges R16 is not fitted, so P4 GPIO5 is not connected to DISPLAY_BL. With the 0 ohm bridge fitted
// the P4 could keep the backlight lit while the C6 (which drives the same net) is in reset or its ROM. Pure decisions,
// host-tested in host_tests/r16_hold_test.c; r16_hold_glue (in backlight.c) does the GPIO work.

typedef enum { R16_UNKNOWN = 0, R16_FITTED = 1, R16_ABSENT = 2 } r16_fit_t; // NVS value of "r16"

typedef enum {
    R16_C6_APP,   // the C6 application runs (or may run): it drives DISPLAY_BL
    R16_C6_RESET, // EN is held low
    R16_C6_ROM,   // proven in download mode (the ROM answered), no application
} r16_c6_t;

// Detection: the C6 drives the backlight PWM at these display levels (the C6 maps 10..80 % straight to duty); GPIO5 is
// sampled as a plain input and the share of high samples is compared with the duty
#define R16_LEVELS 3
extern uint8_t const r16_detect_level[R16_LEVELS]; // 10, 40, 80

typedef struct {
    uint32_t high[R16_LEVELS];
    uint32_t total[R16_LEVELS];
} r16_samples_t;

// FITTED only when the share of high samples follows the duty at every level (within 15 points), ABSENT when it does
// not (floating, stuck low or high, noise), UNKNOWN when nothing was sampled
r16_fit_t r16_classify(r16_samples_t const *s);

// The one rule for driving GPIO5: the bridge was detected AND the C6 cannot be driving the net. Anything else, a stored
// value of UNKNOWN or ABSENT included, never drives.
bool r16_may_drive(r16_fit_t stored, r16_c6_t c6);

const char *r16_fit_name(r16_fit_t f);
