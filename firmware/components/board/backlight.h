#pragma once

#include <stdint.h>

// Display and keyboard backlight (0-100), driven by the C6 (its GPIO15/GPIO10 PWM); the P4 sends "BL <d> <k>" on the
// C6's console UART. Saved in NVS; backlight_start() applies the saved levels. Call after the C6 flasher is done. Also registers
// the restart hook that switches the backlight and panel off before esp_restart() (#23).
void backlight_start(void);
// Applies the levels now, saves nothing (a slider being dragged)
void backlight_set(uint8_t display, uint8_t keyboard);
// Screen timeout (#43): its own levels (dim, or 0 0 = off), not clamped to BACKLIGHT_DISPLAY_MIN, never saved and not
// taken as the person's levels. backlight_reapply() keeps them until backlight_resume() or a backlight_set*().
void backlight_idle(uint8_t display, uint8_t keyboard);
// Back to the person's levels: the last ones backlight_set*() asked for (a slider not yet saved included), from RAM
void backlight_resume(void);
// Counts backlight_set() / backlight_set_saved() calls: the screen timeout takes a change as activity (fw bl, a slider)
uint32_t backlight_user_generation(void);
// Applies and saves (NVS bl_disp / bl_kbd, what "fw bl" writes)
void backlight_set_saved(uint8_t display, uint8_t keyboard);
// The saved levels, or the C6 firmware's defaults before any were saved
void backlight_get(uint8_t *display, uint8_t *keyboard);
// Sends the saved levels once and closes the UART again, for the boot screen before the C6 check (the C6 flasher
// needs the UART driver to itself). A restart leaves the backlight off (see backlight_start()); call this after the
// first frame is drawn.
void backlight_restore(void);
// Power off (#65): both backlights off, then the panel, as before a restart. The C6 must still run (it drives them).
void backlight_power_off(void);
// Sends the last applied levels again (the saved ones if none were applied yet): the C6 starts at its defaults after a
// reset (#14). Call after backlight_start(), once the C6 runs its application again.
void backlight_reapply(void);
