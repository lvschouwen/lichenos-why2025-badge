#pragma once

#include <stdint.h>

// Display and keyboard backlight (0-100), driven by the C6 (its GPIO15/GPIO10 PWM); the P4 sends "BL <d> <k>" on the
// C6's console UART. Saved in NVS; backlight_start() applies the saved levels. Call after the C6 flasher is done.
void backlight_start(void);
// Applies the levels now, saves nothing (a slider being dragged)
void backlight_set(uint8_t display, uint8_t keyboard);
// Applies and saves (NVS bl_disp / bl_kbd, what "fw bl" writes)
void backlight_set_saved(uint8_t display, uint8_t keyboard);
// The saved levels, or the C6 firmware's defaults before any were saved
void backlight_get(uint8_t *display, uint8_t *keyboard);
