#pragma once

#include <stdint.h>

// Display and keyboard backlight (0-100), driven by the C6 (its GPIO15/GPIO10 PWM); the P4 sends "BL <d> <k>" on the
// C6's console UART. Saved in NVS; backlight_start() applies the saved levels. Call after the C6 flasher is done. Also registers
// the restart hook that switches the backlight and panel off before esp_restart() (#23).
void backlight_start(void);
// Applies the levels now, saves nothing (a slider being dragged)
void backlight_set(uint8_t display, uint8_t keyboard);
// Screen timeout (#43): both backlights off, below BACKLIGHT_DISPLAY_MIN, nothing saved. backlight_reapply() keeps them
// off until backlight_set() lights them again.
void backlight_off(void);
// Applies and saves (NVS bl_disp / bl_kbd, what "fw bl" writes)
void backlight_set_saved(uint8_t display, uint8_t keyboard);
// The saved levels, or the C6 firmware's defaults before any were saved
void backlight_get(uint8_t *display, uint8_t *keyboard);
// Sends the saved levels once and closes the UART again, for the boot screen before the C6 check (the C6 flasher
// needs the UART driver to itself). A restart leaves the backlight off (see backlight_start()); call this after the
// first frame is drawn.
void backlight_restore(void);
// Sends the last applied levels again (the saved ones if none were applied yet): the C6 starts at its defaults after a
// reset (#14). Call after backlight_start(), once the C6 runs its application again.
void backlight_reapply(void);
