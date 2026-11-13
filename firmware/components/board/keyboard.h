#pragma once

#include "key_raw.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

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

// The remote key lock (#190): called for every key event before everything else (the wake hook included). True = the
// event is dropped: nothing sees it, so the screen does not even wake. The gate must drop the release of every press
// it dropped. reset is told when the chip was reset and every key counts as released (no releases follow). Keyboard
// task, keep both short; NULL removes them.
void keyboard_set_gate_hook(bool (*fn)(uint8_t key, bool pressed), void (*reset)(void));

// Feeds one key event through the keyboard task's own path (#375), as if the chip had sent it: the wake hook, the raw
// hooks, the mapping to LVGL, and the held-key bookkeeping, so a chip reset releases an injected key too. code is a
// TCA8418 code (1-0x50, kbd_map_code_for); send the release yourself. as_physical also puts it through the gate
// (keyboard_set_gate_hook), which then drops it as it drops the keyboard's keys; without it only the gate is skipped.
// The events are handled in order, ahead of the chip's, at once (the poll task wakes for them). False when the
// keyboard is not started or its 16-deep queue is full (the event is then not sent). Any task, never blocks, no LVGL.
bool keyboard_inject(uint8_t code, bool pressed, bool as_physical);
// True when the key event LVGL last read was injected (keyboard_inject, as_physical or not), false for the TCA8418's
// (#391); a release is as injected as its press. Valid only when called synchronously in the LV_EVENT_KEY handler of
// the key being handled (not after lv_async_call, a UI call or a focus change: the next key has overwritten it).
// False before any key. Hand-only paths check it on the press and refuse an injected one. Any task, no LVGL.
bool keyboard_last_injected(void);

// The TCA8418's health for fw soak (#409): -1 before keyboard_start found it, 1 when the last once-a-second readback
// answered (a chip found reset is reconfigured at once), 0 when it did not answer. *resets: readbacks that found it
// reset (C6 EN, #14). Lock-free, any task.
int keyboard_chip_state(uint32_t *resets);

// Raw key hooks (#81, #79): up to KEYBOARD_RAW_MAX; false when full. Called from any task. A hook that swallows an
// event stops the mapped key reaching LVGL (its release too); other hooks still see it. Once keyboard_raw_remove returns
// no call of that hook is running or will start (#251): it waits for one in flight, except when called from inside a
// hook (the keyboard task), where it removes without waiting. Both may sleep (a mutex, a one-tick wait) outside a hook:
// not from an ISR, a critical section or with the scheduler suspended.
#define KEYBOARD_RAW_MAX 4
bool keyboard_raw_add(key_raw_fn fn, void *arg);
void keyboard_raw_remove(key_raw_fn fn, void *arg);
