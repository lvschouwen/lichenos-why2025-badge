#pragma once

#include <stdint.h>

// The display and keyboard backlight's system surface (#226, #216 decision 1): the shell's attention light, screen
// timeout, thermal load shed and Settings > Display go through here, never to board's backlight driver. System
// surface: tools/layers.conf forbids lichen_sys_* in L6, so an app never dims or lights the badge (it raises a notice
// and the OS decides, #164). Levels are 0-80 (the C6's limit; the screen 10-80). The levels reach the C6, which drives the lights, over UART1 (the
// two-chip contract in docs/architecture.md); nothing here changes that. Every call below may be made from any task
// except an interrupt: the applied levels sit under a critical section in the driver, the saved ones go to NVS.

// The screen never goes fully dark (a saved 0 would leave no way to see the slider again): the lowest screen level
// lichen_sys_display_set / _temporary accept. Equal to board's BACKLIGHT_DISPLAY_MIN (checked in lichen_os).
#define LICHEN_SYS_DISPLAY_MIN 10
// The C6's ceiling (a request above is applied as this); the keyboard's 1..9 are applied as 10, 0 is off
#define LICHEN_SYS_DISPLAY_MAX 80

// Applies the person's levels now and saves nothing (a slider being dragged); the screen level is raised to
// LICHEN_SYS_DISPLAY_MIN. Counts as activity for the screen timeout. Any task.
void lichen_sys_display_set(uint8_t display, uint8_t keyboard);
// As lichen_sys_display_set, and saves the levels (what `fw bl` writes). Any task but not an interrupt (NVS write).
void lichen_sys_display_set_saved(uint8_t display, uint8_t keyboard);
// The saved levels, or the defaults before any were saved. Any task.
void lichen_sys_display_get(uint8_t *display, uint8_t *keyboard);
// Screen timeout (#43): the dim or off levels (0 0 = off), not raised to the minimum, never saved and not taken as the
// person's levels. Lasts until lichen_sys_display_resume or a lichen_sys_display_set*. Any task.
void lichen_sys_display_idle(uint8_t display, uint8_t keyboard);
// Back to the person's levels (the last lichen_sys_display_set*, a slider not yet saved included). Ends an idle level
// and a temporary one. Any task.
void lichen_sys_display_resume(void);
// A temporary override (the keyboard pulse for a new message, the flashlight, #164): clamped like _set, never saved
// and not the person's levels. lichen_sys_display_resume ends it. Any task.
void lichen_sys_display_temporary(uint8_t display, uint8_t keyboard);
// Only the keyboard light, keeping the screen level now applied (the screen timeout's dim included). Any task.
void lichen_sys_display_temporary_keyboard(uint8_t keyboard);
// Counts lichen_sys_display_set / _set_saved calls: the screen timeout takes a change as activity. Any task.
uint32_t lichen_sys_display_user_generation(void);
