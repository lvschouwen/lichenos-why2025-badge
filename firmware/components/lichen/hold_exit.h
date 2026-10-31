#pragma once

#include <stdbool.h>
#include <stdint.h>

// Hold-Esc-to-leave for the keyboard test (#81): while the test owns the keys a short Esc is just a key to test, and
// Esc held for HOLD_EXIT_MS leaves. Pure, host-tested in host_tests/hold_exit_test.c. Times are ms ticks (wrap-safe).
#define HOLD_EXIT_MS 2000

typedef struct {
    bool     down;
    bool     fired;
    uint32_t since;
} hold_exit_t;

// Feed every raw event; is_esc says whether it is the Esc key
void hold_exit_event(hold_exit_t *h, bool is_esc, bool pressed, uint32_t now);
// Call regularly. True exactly once per hold, when Esc has been down HOLD_EXIT_MS
bool hold_exit_tick(hold_exit_t *h, uint32_t now);
// 0-100 how far the hold has got, 0 when Esc is up
int  hold_exit_percent(hold_exit_t const *h, uint32_t now);
