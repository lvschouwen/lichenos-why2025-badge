#pragma once

#include <stdbool.h>
#include <stdint.h>

// A raw TCA8418 key event (#81), before kbd_map_key drops the keys LVGL has no value for (shape keys, Fn, Ctrl ...).
// Pure header: also built into the host tests.
typedef struct {
    uint8_t code;    // 1-0x50, without the press bit
    bool    pressed;
    bool    shift;   // a Shift key is down (for a Shift key's own event: after the event)
} key_raw_event_t;

// Runs on the keyboard task for every event, before it is mapped: it must be quick and must not touch LVGL (queue the
// event, see lichen's raw_keys.h). Returns true to swallow the event: it then never reaches LVGL.
typedef bool (*key_raw_fn)(key_raw_event_t const *ev, void *arg);
