#pragma once

#include <stdbool.h>

// What set off the UI work running now (#400): a hand on the keyboard, an injected key ("fw keys", "fw ui answer",
// #375) or an operator's console command ("fw ui open"). A hand-only action (starting MeshCore: a start may send a due
// periodic advert) asks key_cause_now and refuses an injected cause, whichever path the key took (a shape-key
// shortcut, a home tile, the grid, a notification, a Settings row), so a new shortcut cannot get past it.
//   - The shell marks the work it does for one known cause: each raw key's listeners (the shape keys, raw_keys.c) with
//     the event's own origin, and a console command's UI part with KEY_CAUSE_CONSOLE.
//   - Unmarked work is an LVGL key's (a tile's Enter, also one the UI service ran a moment later): the last key LVGL
//     read decides, through the getter the shell binds. Nothing bound counts as injected (fail closed).
// Pure (host_tests/key_cause_test.c). Under the LVGL lock: the LVGL task, or a task holding lichen_sys_ui_lock.

typedef enum {
    KEY_CAUSE_NONE,     // no mark (key_cause_enter's answer when nothing was marked); key_cause_now never returns it
    KEY_CAUSE_HAND,     // the TCA8418: a hand
    KEY_CAUSE_INJECTED, // an injected key, "phys" ones too (key_origin_injected)
    KEY_CAUSE_CONSOLE,  // an explicit console command that does the action itself, not by a key
} key_cause_t;

typedef bool (*key_cause_last_fn)(void); // true when the last key LVGL read was injected

// The shell, once at start: the last LVGL key's origin (lichen_sys_keys_last_injected). Any task; NULL unbinds.
void key_cause_bind_last_key(key_cause_last_fn fn);
// The shell only (tools/layers.conf): marks the work from here on as cause's and returns the mark before, which
// key_cause_leave puts back. LVGL task (or a task holding lichen_sys_ui_lock).
key_cause_t key_cause_enter(key_cause_t cause);
// The shell only: puts back the mark key_cause_enter returned. LVGL task (or a task holding lichen_sys_ui_lock).
void        key_cause_leave(key_cause_t previous);
// The cause of the work running now: the mark, else the last LVGL key's origin. LVGL task (or holding the lock).
key_cause_t key_cause_now(void);
// A hand-only action may go ahead for this cause: a hand or a console command, never an injected key. Pure, any task.
bool key_cause_hand_only_ok(key_cause_t cause);
// "hand", "injected key", "console" ("none"). Pure, any task.
char const *key_cause_name(key_cause_t cause);
