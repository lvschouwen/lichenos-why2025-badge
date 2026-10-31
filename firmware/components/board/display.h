#pragma once

#include "lvgl.h"

#include <stdbool.h>

// The 720x720 panel driven through LVGL (esp_lvgl_port); draw buffers in PSRAM (#61)
bool display_start(lv_display_t **out);
