#pragma once

#include <stdbool.h>
#include <stdint.h>

// The countdown of an OS warning dialog (#14 T-B): Enter confirms, Esc cancels, and when the time runs out it
// confirms by itself. The default at the end is the confirm because the dialog that uses it (the C6 is gone) may be
// shown while the keyboard is dead: C6 EN also resets the TCA8418. Pure, host-tested in
// host_tests/dialog_countdown_test.c. Times are ms ticks (wrap-safe).

typedef enum { DLG_WAIT, DLG_CONFIRM, DLG_CANCEL } dlg_result_t;
typedef enum { DLG_KEY_OTHER, DLG_KEY_ENTER, DLG_KEY_ESC } dlg_key_t;

typedef struct {
    uint32_t start_ms;
    uint32_t total_ms;
    bool     decided; // a result other than DLG_WAIT was handed out: no second one
} dlg_countdown_t;

void         dlg_countdown_start(dlg_countdown_t *d, uint32_t now_ms, uint32_t seconds);
// A key press: Enter confirms, Esc cancels, any other key does nothing. At most one decision per countdown.
dlg_result_t dlg_countdown_key(dlg_countdown_t *d, dlg_key_t key);
// Call regularly: DLG_CONFIRM once the time is up (once)
dlg_result_t dlg_countdown_tick(dlg_countdown_t *d, uint32_t now_ms);
// Whole seconds left, rounded up ("restarting in 30 s" at the start, 1 in the last second, 0 when over)
uint32_t     dlg_countdown_left_s(dlg_countdown_t const *d, uint32_t now_ms);
