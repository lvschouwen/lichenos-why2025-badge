// Backlight level clamps (pure, host_tests/backlight_level_test.c)
#pragma once

#include <stdint.h>

// The screen never goes fully dark: a saved 0 would boot to a black screen with no way to see the slider again
#define BACKLIGHT_DISPLAY_MIN 5

uint8_t backlight_display_level(unsigned value);  // BACKLIGHT_DISPLAY_MIN..100
uint8_t backlight_keyboard_level(unsigned value); // 0..100

// The C6's console line "\nBL <display> <keyboard>\n" (the leading newline ends a partial line). No clamping: the
// shutdown handler sends 0 0 through this, which backlight_display_level() would raise to 5. Returns the length, or
// 0 if it does not fit in size bytes.
#include <stddef.h>
size_t backlight_cmd_format(char *buf, size_t size, unsigned display, unsigned keyboard);
