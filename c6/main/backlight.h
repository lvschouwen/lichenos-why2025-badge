// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
// The display and keyboard backlight PWM on the C6. Two tasks drive it (the UART "BL d k" line and the registry on the
// LoRa task, #309), so every call below takes one lock around the LEDC.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { BL_DISPLAY, BL_KEYBOARD, BL_COUNT };

// LEDC timer and both channels at the boot levels (display 30 %, keyboard 10 %). Call once, before the other tasks.
void backlight_start(void);

// Set one backlight to percent (0..100, backlight_duty's 10-80 % clamp applies); false when the LEDC refused it
bool backlight_apply(int which, int percent);

// The brightness the channel drives now, from the LEDC duty register; false when the LEDC cannot tell
bool backlight_applied(int which, uint8_t *percent);

// The PWM frequency the timer runs, 0 when the LEDC cannot tell
uint32_t backlight_freq_hz(void);

// True until the first backlight_apply since boot: the levels are still the C6's own, no P4 has set them
bool backlight_at_boot(void);

#ifdef __cplusplus
}
#endif
