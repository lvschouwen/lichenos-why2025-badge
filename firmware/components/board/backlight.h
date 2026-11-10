#pragma once

#include "backlight_level.h"

#include <stdbool.h>
#include <stdint.h>

// Display and keyboard backlight (0-100), driven by the C6 (its GPIO15/GPIO10 PWM); the P4 sends "BL <d> <k>" on the
// C6's console UART, or REG_SET through the settings registry when the C6 has it (#310). Saved by the settings service; backlight_start() applies the saved levels. Call after the C6 flasher is done. Also registers
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
// The levels last sent to the C6 (a dim, a pulse or a slider included), the saved ones before any send: the settings
// registry's "requested" for bl.display / bl.keyboard (#310). backlight_get stays the saved levels: thermal's restore
// and the Settings slider start from what the person set, never from a dimmed level.
void backlight_get_requested(uint8_t *display, uint8_t *keyboard);
// Which call set the levels now (#310, read-only registry keys bl.idle / bl.override): the screen timeout's dim or off,
// and the temporary overrides (backlight_temporary = the flashlight, backlight_temporary_keyboard = the message pulse).
// A backlight_set*, resume or restore ends both. Thermal's load shed goes through backlight_set: the registry adds it.
typedef enum { BACKLIGHT_BRIGHT, BACKLIGHT_DIM, BACKLIGHT_OFF } backlight_idle_t;
typedef enum { BACKLIGHT_OVR_NONE, BACKLIGHT_OVR_FLASHLIGHT, BACKLIGHT_OVR_PULSE } backlight_override_t;
void backlight_mode(backlight_idle_t *idle, backlight_override_t *ovr);
// The settings registry's path to the C6 (#310): when bound, every level change is offered to it first; true = it
// took them (REG_SET bl.display / bl.keyboard, the C6 has the registry's bl domain), false = the UART1 BL line as
// before. Called on the sender's task, the LVGL task included: it must not block. The restart blank, the UART lend's
// return and backlight_reapply always use the UART line.
// The restart blank (and the power off) first calls close: it must drop the levels still waiting for the C6 and refuse
// new ones, so no REG_SET can land after the UART BL 0 0 and light the panel again; from then on every level goes
// over UART1. close may wait briefly (a send in flight), never long.
typedef bool (*backlight_reg_send_fn)(uint8_t display, uint8_t keyboard);
typedef void (*backlight_reg_close_fn)(void);
void backlight_bind_reg_send(backlight_reg_send_fn send, backlight_reg_close_fn close);
// "fw bl r16" (the registry's fw bl alias calls it): detect R16 and save the result, replies on the console
void backlight_r16_console(void);
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
// UART1 lend (#250): the C6 flasher and "fw c6log" borrow the backlight's UART for their whole use, one at a time (false
// when it is already lent). Exclusive (the flasher): the backlight's driver is deleted (the flasher installs its own),
// the backlight never installs nor writes, and the newest levels asked for meanwhile go out on return. Shared (c6log,
// which only reads): the backlight's driver is installed if it was not and stays for the lend; the borrower reads
// through it and never installs or deletes one, and BL lines keep going out. Senders never block for the lend's length.
bool backlight_uart_lend(bool exclusive);
void backlight_uart_return(void);
// c6_flasher brackets a whole flash check with this: "fw bl r16" refuses meanwhile
void backlight_r16_flash(bool on);
