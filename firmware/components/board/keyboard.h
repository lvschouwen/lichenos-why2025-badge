#pragma once

#include "key_raw.h"
#include "lvgl.h"

#include <stdbool.h>

// The badge keyboard (TCA8418) as an LVGL keypad. Start it after the C6 flasher: resetting the C6 (GPIO12) also resets
// the TCA8418 (#15, #61).
bool keyboard_start(lv_indev_t **out);

// Raw key hooks (#81, #79): up to KEYBOARD_RAW_MAX; false when full. Called from any task. A hook that swallows an
// event stops the mapped key reaching LVGL (its release too); other hooks still see it.
#define KEYBOARD_RAW_MAX 4
bool keyboard_raw_add(key_raw_fn fn, void *arg);
void keyboard_raw_remove(key_raw_fn fn, void *arg);
