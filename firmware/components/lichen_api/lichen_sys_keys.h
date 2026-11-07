#pragma once

#include "key_raw.h"

#include <stdbool.h>
#include <stdint.h>

// The keyboard's hooks, the shell's side (#244, #216 decision 1): raw key hooks, the screen wake hook and the remote
// key gate, never board's keyboard.h. System surface: tools/layers.conf forbids lichen_sys_* in L6 (an app's raw keys
// are lichen_keys.h).

// The event and hook types are key_raw.h's (fw_console, L0): board and the gate share the one definition. A hook runs
// on the keyboard task for every event, before it is mapped: quick, and never LVGL. True swallows the event; the first
// hook to swallow it stops the mapped key reaching LVGL (its release too).

// Adds a raw key hook (false when all are taken). Any task.
bool lichen_sys_keys_hook_add(key_raw_fn fn, void *arg);
// Removes a hook added with the same fn and arg. Any task.
void lichen_sys_keys_hook_remove(key_raw_fn fn, void *arg);
// Called first for every key press: true = the press only woke the screen (nothing else sees it, nor its release).
// Keyboard task, keep it short; NULL removes it. Any task to set.
void lichen_sys_keys_set_wake(bool (*fn)(void));
// The remote key lock: called for every key event before everything else, true drops it. reset is told when the chip
// was reset and every key counts as released. Keyboard task, keep both short; NULL removes them. Any task to set.
void lichen_sys_keys_set_gate(bool (*fn)(uint8_t key, bool pressed), void (*reset)(void));
