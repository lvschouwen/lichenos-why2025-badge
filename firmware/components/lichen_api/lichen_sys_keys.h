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

// Adds a raw key hook (false when all are taken or fn is NULL). Any task.
bool lichen_sys_keys_hook_add(key_raw_fn fn, void *arg);
// Removes a hook added with the same fn and arg. Any task. On return no call of the hook is running or will start
// (#251): it waits for one in flight, so never call it holding something the hook waits for. From inside a hook it
// removes without waiting. Outside a hook add and remove may sleep (a mutex, a one-tick wait): never from an ISR, a
// critical section or with the scheduler suspended.
void lichen_sys_keys_hook_remove(key_raw_fn fn, void *arg);
// Called first for every key press: true = the press only woke the screen (nothing else sees it, nor its release).
// Keyboard task, keep it short; NULL removes it. Any task to set.
void lichen_sys_keys_set_wake(bool (*fn)(void));
// The remote key lock: called for every key event before everything else, true drops it. reset is told when the chip
// was reset and every key counts as released. Keyboard task, keep both short; NULL removes them. Any task to set.
void lichen_sys_keys_set_gate(bool (*fn)(uint8_t key, bool pressed), void (*reset)(void));

// Key injection for the console's "fw keys send/press" (#375; meant for the console, the manifest's capability column
// is per header). Feeds one event of a TCA8418 code (1-0x50) through the keyboard task's own path, as the chip's: the
// wake hook, the raw hooks, LVGL and the held-key bookkeeping. as_physical also puts it through the remote key gate,
// which drops it as it drops the keyboard's keys; without it only the gate is skipped. The caller sends the release.
// False when there is no keyboard or its 16-deep queue is full. Any task, never blocks.
bool    lichen_sys_keys_inject(uint8_t code, bool pressed, bool as_physical);
// Hand-only = TCA8418 only (#391): a DANGER confirm or a mode switch that must take a hand on the keyboard refuses
// injected keys, "phys" ones included. A raw hook reads ev->injected; a shell key handler (LV_EVENT_KEY and the like)
// asks this: true when the key event LVGL last read, the one it is handling, came from lichen_sys_keys_inject. False
// before any key. Any task, but only meaningful on the LVGL task while it handles a key.
bool    lichen_sys_keys_last_injected(void);
// The code that types key (an LVGL key or a character), 0 when no key types it. *shift_code is the Shift key's code
// when Shift must be down for it, else 0. Pure, any task.
uint8_t lichen_sys_keys_code_for(uint32_t key, uint8_t *shift_code);
