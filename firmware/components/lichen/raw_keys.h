#pragma once

#include "key_raw.h"

#include <stdbool.h>

// Raw key events in LVGL context (#81; #79's shape-key shortcuts use the same). board/keyboard.c hands every event to a
// hook on the keyboard task; this queues them and delivers them from an LVGL timer, so listeners may touch LVGL.
// Listener calls come in order, at most about 20 ms late. All functions: LVGL context only.
typedef void (*raw_keys_cb)(key_raw_event_t const *ev, void *arg);

// swallow: while this listener is registered the events do not also act as normal keys (see keyboard_raw_add).
// False when the listener table is full.
bool raw_keys_listen(raw_keys_cb cb, void *arg, bool swallow);
void raw_keys_unlisten(raw_keys_cb cb, void *arg);
