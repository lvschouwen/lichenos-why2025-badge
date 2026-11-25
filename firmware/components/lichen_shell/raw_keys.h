#pragma once

#include "key_raw.h"
#include "lichen_app.h"

#include <stdbool.h>

// Raw key events in LVGL context (#81; #79's shape-key shortcuts use the same). board/keyboard.c hands every event to a
// hook on the keyboard task; this queues them and delivers them from an LVGL timer, so listeners may touch LVGL.
// Listener calls come in order, at most about 20 ms late. All functions: LVGL context only.
typedef void (*raw_keys_cb)(key_raw_event_t const *ev, void *arg);

// swallow: while this listener is registered the events do not also act as normal keys (see lichen_sys_keys_hook_add).
// False when the listener table is full.
// lichen_keys_raw_take sets this while an app (the Keyboard Test) holds the keys; the keyboard task stamps it into
// each event at capture time
void raw_keys_set_test_open(bool open);
bool raw_keys_test_open(void); // what the keyboard task stamps now
// Any app holds the raw keys (the Keyboard test or a game, #534): it owns every key, so the shape-key shortcuts and the
// remote "ui open" stay off. Only the Keyboard test also sets raw_keys_test_open.
bool raw_keys_held(void);
// Frees the raw keys when ctx holds them, nothing otherwise (#534, for #533's unload and card removal). OS-internal, not
// exported. Any task: it takes the LVGL lock itself (recursive, so the LVGL task may call it too). The caller must not
// hold lichen_sys_app_lock: the lock order is LVGL, then the app lock. True when ctx does not hold the keys afterwards
// (it never did, or they are free now); false when the LVGL lock could not be taken, so the callback may still run:
// do not free the image then.
bool raw_keys_release_ctx(lichen_ctx_t ctx);

// The listener table has no lock of its own: callers of raw_keys_listen / raw_keys_unlisten hold the LVGL lock
// (lichen_sys_ui_lock, e.g. from a console command) or run on the LVGL task (event handlers, timers), which is what keeps
// them from racing the drain timer and each other (#118)
bool raw_keys_listen(raw_keys_cb cb, void *arg, bool swallow);
void raw_keys_unlisten(raw_keys_cb cb, void *arg);

// The OS hold-Esc for a video owner (#563): hooks the keyboard task for Esc and starts the sampler on the LVGL task. Esc
// held VIDEO_ESCAPE_MS while a video owner is active takes the screen back (reason escape), gives the owner's raw keys back
// and leaves the app (shell_back). A short Esc is an ordinary key for the app. Call once from the shell's start, under the
// LVGL lock.
void raw_keys_start_escape(void);
