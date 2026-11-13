#pragma once

#include <stdbool.h>

// Where a key event came from (#391). Only the TCA8418 is a hand on the keyboard: "fw keys send" and "fw keys press"
// (#375) are injected, and so is a "phys" key, which only means it also goes through the remote key gate. Hand-only
// paths (the #66 factory question, About's developer taps) refuse every injected event. Pure (host_tests/
// key_origin_test.c).
typedef enum {
    KEY_ORIGIN_CHIP,         // the TCA8418, or the keyboard's own release of the chip's held keys after a chip reset
    KEY_ORIGIN_INJECTED,     // keyboard_inject without as_physical: skips the remote key gate
    KEY_ORIGIN_INJECTED_GATE // keyboard_inject with as_physical: through the gate like the chip's keys
} key_origin_t;

key_origin_t key_origin_of_inject(bool as_physical);
// The remote key gate (#190) sees it
bool key_origin_gated(key_origin_t o);
// Not a hand: the raw event's injected flag and lichen_sys_keys_last_injected
bool key_origin_injected(key_origin_t o);
