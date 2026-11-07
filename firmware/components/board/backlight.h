#pragma once

#include "backlight_level.h"

#include <stdbool.h>
#include <stdint.h>

// Display and keyboard backlight (0-100), driven by the C6 (its GPIO15/GPIO10 PWM); the P4 sends "BL <d> <k>" on the
// C6's console UART. Saved by the settings service; backlight_start() applies the saved levels. Call after the C6 flasher is done. Also registers
// the restart hook that switches the backlight and panel off before esp_restart() (#23).
void backlight_start(void);
// Where the levels and the R16 result are kept (the settings service, #224): board opens no NVS. Call before
// backlight_restore(); until then the defaults apply and nothing is saved.
void backlight_bind_store(backlight_get_u8_fn get, backlight_set_u8_fn set);
// Applies the levels now, saves nothing (a slider being dragged)
void backlight_set(uint8_t display, uint8_t keyboard);
// Screen timeout (#43): its own levels (dim, or 0 0 = off), not clamped to BACKLIGHT_DISPLAY_MIN, never saved and not
// taken as the person's levels. backlight_reapply() keeps them until backlight_resume() or a backlight_set*().
void backlight_idle(uint8_t display, uint8_t keyboard);
// Back to the person's levels: the last ones backlight_set*() asked for (a slider not yet saved included), from RAM
void backlight_resume(void);
// A temporary override (#164: the keyboard pulse for a new message, the flashlight): sent like backlight_idle() but
// clamped like backlight_set(), never taken as the person's levels, never saved. backlight_resume() ends it. The keyboard
// form keeps the screen level now applied (the screen timeout's dim included).
void backlight_temporary(uint8_t display, uint8_t keyboard);
void backlight_temporary_keyboard(uint8_t keyboard);
// Counts backlight_set() / backlight_set_saved() calls: the screen timeout takes a change as activity (fw bl, a slider)
uint32_t backlight_user_generation(void);
// Applies and saves (what "fw bl" writes)
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

// R16 (#12): P4 GPIO5 can drive DISPLAY_BL only when the R16 bridge is fitted. Detection is read-only ("fw bl r16": GPIO5 as
// an input while the C6 PWMs at three levels) and stored in the settings. backlight_r16_hold() drives GPIO5 high ONLY when the
// stored result says fitted; the caller must have proof that the C6 runs no application (ROM answered). The caller
// holds C6 EN low, THEN calls backlight_r16_release(), THEN lets EN rise. On a badge without R16 both do nothing.
void backlight_r16_hold(void);
void backlight_r16_release(void);
// c6_flasher brackets a whole flash check with this: "fw bl r16" refuses meanwhile
void backlight_r16_flash(bool on);
