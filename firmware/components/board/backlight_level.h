// Backlight level clamps (pure, host_tests/backlight_level_test.c)
#pragma once

#include <stdint.h>

// The screen never goes fully dark: a saved 0 would boot to a black screen with no way to see the slider again
#define BACKLIGHT_DISPLAY_MIN 5

uint8_t backlight_display_level(unsigned value);  // BACKLIGHT_DISPLAY_MIN..100
uint8_t backlight_keyboard_level(unsigned value); // 0..100
