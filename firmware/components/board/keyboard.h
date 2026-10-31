#pragma once

#include "lvgl.h"

#include <stdbool.h>

// The badge keyboard (TCA8418) as an LVGL keypad. Start it after the C6 flasher: resetting the C6 (GPIO12) also resets
// the TCA8418 (#15, #61).
bool keyboard_start(lv_indev_t **out);
