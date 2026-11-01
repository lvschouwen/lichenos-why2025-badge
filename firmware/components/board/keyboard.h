#pragma once

#include "key_raw.h"
#include "lvgl.h"

#include <stdbool.h>

// The badge keyboard (TCA8418) as an LVGL keypad. Start it after the C6 flasher: resetting the C6 (GPIO12) also resets
// the TCA8418 (#15, #61).
bool keyboard_start(lv_indev_t **out);
// Configure the TCA8418 again (#14), e.g. after the C6 was reset: the keyboard task writes its registers, drops what
// the chip had queued and releases every held key. Returns at once; safe from any task, no LVGL call.
void keyboard_reinit(void);
// Called from the keyboard task when its once-a-second readback found the chip reset (and reconfigured it), which
// means C6 EN went low (#14). Keep it short; NULL removes it.
void keyboard_set_reset_hook(void (*fn)(void));

// Screen timeout (#43): called first for every key press, before the raw hooks. True = the press only woke the screen:
// no raw hook and not LVGL sees it, nor its release. Keyboard task, keep it short; NULL removes it.
void keyboard_set_wake_hook(bool (*fn)(void));

// Raw key hooks (#81, #79): up to KEYBOARD_RAW_MAX; false when full. Called from any task. A hook that swallows an
// event stops the mapped key reaching LVGL (its release too); other hooks still see it.
#define KEYBOARD_RAW_MAX 4
bool keyboard_raw_add(key_raw_fn fn, void *arg);
void keyboard_raw_remove(key_raw_fn fn, void *arg);
