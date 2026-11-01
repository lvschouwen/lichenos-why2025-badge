#pragma once

#include <stdint.h>

// Inclusive pixel rectangle, LVGL style (x2/y2 are the last pixel, not one past it)
typedef struct {
    int32_t x1, y1, x2, y2;
} display_rect_t;

// Where a strip LVGL rendered for LV_DISPLAY_ROTATION_90 lands on the panel. `ui` is the flush area in UI
// (rotated) coordinates, `ui_hres` the UI width (= the panel height). LVGL's 90 turns the picture a quarter
// counter-clockwise (lv_draw_sw_rotate, PPA_SRM_ROTATION_ANGLE_90): UI pixel (x, y) goes to panel
// (y, ui_hres - 1 - x). Same maths as esp_lvgl_port's lvgl_port_rotate_area and lvgl_port_ppa_rotate (#95).
display_rect_t display_rot90_block(display_rect_t ui, int32_t ui_hres);
