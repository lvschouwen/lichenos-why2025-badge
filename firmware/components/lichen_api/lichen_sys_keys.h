#pragma once

#include <stdbool.h>
#include <stdint.h>

// The keyboard's hooks, the shell's side (#244, #216 decision 1): raw key hooks, the screen wake hook and the remote
// key gate, never board's keyboard.h. System surface: tools/layers.conf forbids lichen_sys_* in L6 (an app's raw keys are
// lichen_keys.h). The event repeats board's key_raw_event_t (the gate cannot include it); lichen_sys_pure_view.c keeps
// them equal with asserts.

typedef struct {
    uint8_t code;      // 1-0x50, without the press bit
    bool    pressed;
    bool    test_open; // stamped at capture: the Keyboard Test was the foreground screen
    bool    shift;     // a Shift key is down (for a Shift key's own event: after the event)
} lichen_sys_key_event_t;

// Runs on the keyboard task for every event, before it is mapped: quick, and never LVGL. True swallows the event.
typedef bool (*lichen_sys_key_fn)(lichen_sys_key_event_t const *ev, void *arg);

#define LICHEN_SYS_KEY_HOOKS_MAX 4 // raw hooks at once

// Adds a raw key hook (false when all are taken) or removes it again. Any task.
bool lichen_sys_keys_hook_add(lichen_sys_key_fn fn, void *arg);
void lichen_sys_keys_hook_remove(lichen_sys_key_fn fn, void *arg);
// Called first for every key press: true = the press only woke the screen (nothing else sees it, nor its release).
// Keyboard task, keep it short; NULL removes it. Any task to set.
void lichen_sys_keys_set_wake(bool (*fn)(void));
// The remote key lock: called for every key event before everything else, true drops it. reset is told when the chip was
// reset and every key counts as released. Keyboard task, keep both short; NULL removes them. Any task to set.
void lichen_sys_keys_set_gate(bool (*fn)(uint8_t key, bool pressed), void (*reset)(void));
