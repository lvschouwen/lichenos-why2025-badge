#pragma once

#include "lvgl.h"

#include <stdbool.h>

// The 720x720 panel driven through LVGL (esp_lvgl_port); draw buffers in PSRAM (#61)
bool display_start(lv_display_t **out);

// Switches the panel off (display-off command; a later display_start() resets and re-inits it). Safe from a shutdown
// handler; does nothing before display_start() succeeded.
void display_blank(void);
