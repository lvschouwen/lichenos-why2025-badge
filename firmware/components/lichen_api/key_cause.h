#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// What set off the UI work running now (#400): a hand on the keyboard, an injected key ("fw keys", "fw ui answer",
// #375) or no key at all (an operator's console command such as "fw ui open", or the OS restoring what ran). A
// hand-only action (starting MeshCore: a start may send a due periodic advert) asks key_cause_now and refuses an
// injected cause, whichever path the key took (a shape-key shortcut, a home tile, the grid, a notification, a Settings
// row), so a new shortcut cannot get past it.
//   - The shell marks the work it does for one known cause: each raw key's listeners (the shape keys, raw_keys.c) with
//     the event's own origin, and a console command's UI part with KEY_CAUSE_SYSTEM.
//   - A deferred call (lichen_ui_call, lichen_sys_ui_call) records key_cause_now() when it is queued and runs under
//     that mark (ui_call_queue.c), so it is judged by the key that queued it, not by the key read last.
//   - Unmarked work asks the getter the shell binds: the key LVGL is handling, only while an input device is being
//     read (key_cause_last_key_injected); a timer or an async call with no mark counts as injected (fail closed), and
//     so does nothing bound.
// Pure (host_tests/key_cause_test.c). Under the LVGL lock: the LVGL task, or a task holding lichen_sys_ui_lock.

typedef enum {
    KEY_CAUSE_NONE,     // no mark (key_cause_enter's answer when nothing was marked); key_cause_now never returns it
    KEY_CAUSE_HAND,     // the TCA8418: a hand
    KEY_CAUSE_INJECTED, // an injected key, "phys" ones too (key_origin_injected), or work of no known cause
    KEY_CAUSE_SYSTEM,   // no key: an operator's console command that does the action itself, or the OS
} key_cause_t;

typedef bool (*key_cause_last_fn)(void); // true unless a hand's key is being handled right now

// The shell, once at start: its getter for the key being handled (built on key_cause_last_key_injected). Any task; NULL
// unbinds.
void key_cause_bind_last_key(key_cause_last_fn fn);
// The shell and the UI service only (tools/layers.conf): marks the work from here on as cause's and returns the mark
// before, which key_cause_leave puts back. LVGL task (or a task holding lichen_sys_ui_lock).
key_cause_t key_cause_enter(key_cause_t cause);
// The shell and the UI service only: puts back the mark key_cause_enter returned. LVGL task (or holding the lock).
void        key_cause_leave(key_cause_t previous);
// The cause of the work running now: the mark, else the bound getter. LVGL task (or holding the lock).
key_cause_t key_cause_now(void);
// The shell's getter, pure: an LVGL key is a hand's only while LVGL reads an input device (indev_active) and the key it
// read last (lichen_sys_keys_last_injected, true before the first key) was the chip's. Pure, any task.
bool key_cause_last_key_injected(bool indev_active, bool last_injected);
// A hand-only action may go ahead for this cause: a hand or no key (SYSTEM), never an injected key. Pure, any task.
bool key_cause_hand_only_ok(key_cause_t cause);
// "hand", "injected key", "system" ("none"). Pure, any task.
char const *key_cause_name(key_cause_t cause);

#ifdef __cplusplus
}
#endif
