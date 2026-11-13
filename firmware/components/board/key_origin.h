#pragma once

#include <stdbool.h>

// Where a key event came from (#391). Only the TCA8418 is a hand on the keyboard: "fw keys send" and "fw keys press"
// (#375) are injected, and so is a "phys" key, which only means it also goes through the remote key gate. Hand-only
// paths (the #66 factory question, About's developer taps) refuse every injected event. Pure (host_tests/
// key_origin_test.c).
typedef enum {
    KEY_ORIGIN_NONE,         // a key's slot with no press on record (key_origin_stamp)
    KEY_ORIGIN_CHIP,         // the TCA8418
    KEY_ORIGIN_INJECTED,     // keyboard_inject without as_physical: skips the remote key gate
    KEY_ORIGIN_INJECTED_GATE // keyboard_inject with as_physical: through the gate like the chip's keys
} key_origin_t;

key_origin_t key_origin_of_inject(bool as_physical);
// The remote key gate (#190) sees it
bool key_origin_gated(key_origin_t o);
// Not a hand: the raw event's injected flag and lichen_sys_keys_last_injected
bool key_origin_injected(key_origin_t o);
// The origin is per key, not per event: a press records its own in the key's slot (*held) and reports it; a release
// reports the press's and clears the slot, whichever path the release came by (the chip, an injection, the release of
// every held key after a chip reset). So a gesture is never part hand and part injected. A release with no press on
// record reports its own origin.
key_origin_t key_origin_stamp(key_origin_t *held, bool pressed, key_origin_t origin);
