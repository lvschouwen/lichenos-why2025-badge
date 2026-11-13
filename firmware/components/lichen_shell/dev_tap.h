#pragma once

#include <stdbool.h>
#include <stdint.h>

// The hidden way into developer mode (#374): DEV_TAP_COUNT taps on the About page, each within DEV_TAP_GAP_MS of the
// one before. A slower tap starts the count again at 1. Pure, host-tested in host_tests/dev_tap_test.c; now is a ms
// tick (wrap-safe).
#define DEV_TAP_COUNT  7
#define DEV_TAP_GAP_MS 3000

typedef struct {
    uint8_t  n;    // taps counted so far
    uint32_t last; // tick of the latest one
} dev_tap_t;

void dev_tap_reset(dev_tap_t *t);
// One tap. True exactly when it is the DEV_TAP_COUNT-th in a row; the count then starts over.
bool dev_tap(dev_tap_t *t, uint32_t now);

typedef enum { DEV_ENTER_NONE, DEV_ENTER_SWITCH_ON, DEV_ENTER_ASK_OFF, DEV_ENTER_REFUSED } dev_enter_action_t;
// What an Enter press on About does: while on it asks to switch off (the count stays at 0), while off it is a tap and
// the seventh switches on. An injected press (lichen_sys_keys_last_injected, #391) is REFUSED and leaves t as it was:
// developer mode is switched on by a hand or over USB only (#373), so keys sent over Wi-Fi neither count nor ask.
dev_enter_action_t dev_enter_action(dev_tap_t *t, bool dev_on, bool injected, uint32_t now);
