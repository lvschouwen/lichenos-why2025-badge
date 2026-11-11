// Backlight level clamps (pure, host_tests/backlight_level_test.c)
#pragma once

#include <stdbool.h>
#include <stdint.h>

// The screen never goes fully dark: a saved 0 would boot to a black screen with no way to see the slider again
#define BACKLIGHT_DISPLAY_MIN 10 // since #332 the C6's floor (a lower request is applied as 10): saved = applied
// The C6 holds every nonzero backlight level to 10..80 (backlight_duty in c6/main/backlight_cmd.c: the C6 image shares no
// header with the P4, so these are copies; keep them equal). The screen slider and the saved levels stay inside, so what
// is saved is what is applied; a saved value outside (an older image) is clamped on read, never rewritten.
#define BACKLIGHT_DISPLAY_MAX 80
#define BACKLIGHT_KEYBOARD_MIN 10 // 0 is off
#define BACKLIGHT_KEYBOARD_MAX 80

uint8_t backlight_display_level(unsigned value);  // BACKLIGHT_DISPLAY_MIN..BACKLIGHT_DISPLAY_MAX
uint8_t backlight_keyboard_level(unsigned value); // 0, else BACKLIGHT_KEYBOARD_MIN..BACKLIGHT_KEYBOARD_MAX

// The C6's console line "\nBL <display> <keyboard>\n" (the leading newline ends a partial line). No clamping: the
// shutdown handler sends 0 0 through this, which backlight_display_level() would raise to the minimum. Returns the length, or
// 0 if it does not fit in size bytes.
#include <stddef.h>
size_t backlight_cmd_format(char *buf, size_t size, unsigned display, unsigned keyboard);

// The C6 firmware's start levels: what a badge with nothing saved gets
#define BACKLIGHT_DEFAULT_DISPLAY  30
#define BACKLIGHT_DEFAULT_KEYBOARD 10

// The levels live in the settings service (board opens no NVS, #224); lichen_os binds its u8 getter and setter here.
// Keys are the badge_fw ones from before the move, so a saved level stays valid.
#define BACKLIGHT_KEY_DISPLAY  "bl_disp"
#define BACKLIGHT_KEY_KEYBOARD "bl_kbd"
#define BACKLIGHT_KEY_R16      "r16"
typedef uint8_t (*backlight_get_u8_fn)(char const *key, uint8_t def);
typedef bool (*backlight_set_u8_fn)(char const *key, uint8_t value);

// The saved levels, clamped (an older build could save a screen level of 0: a black screen). A NULL getter, or a missing
// key, gives the defaults.
void backlight_levels_load(backlight_get_u8_fn get, uint8_t *display, uint8_t *keyboard);
// Saves the (already clamped) levels; false when there is no setter or a write failed
bool backlight_levels_save(backlight_set_u8_fn set, uint8_t display, uint8_t keyboard);
