#pragma once

#include <stdbool.h>
#include <stdint.h>

// The pocket lock's unlock combo (#75): Esc and Backspace, the two ends of the top row, held together for LOCK_HOLD_MS
// with no other key down. A pocket presses keys in clumps, rarely exactly these two at once and long enough. Pure,
// host-tested in host_tests/lock_combo_test.c. Codes are TCA8418 key codes (1-0x50), times ms ticks (wrap-safe).
#define LOCK_KEY_A   0x01 // Esc
#define LOCK_KEY_B   0x08 // Backspace
#define LOCK_HOLD_MS 1000
#define LOCK_COMBO_TEXT "Esc + Backspace"

typedef struct {
    uint8_t  down[11]; // a bit per key code 0-0x57
    bool     armed;    // both down, nothing else: counting since `since`
    bool     fired;    // fired for this hold; both released re-arm it
    uint32_t since;
} lock_combo_t;

void lock_combo_reset(lock_combo_t *c);
// Feed every raw key event
void lock_combo_event(lock_combo_t *c, uint8_t code, bool pressed, uint32_t now);
// Call regularly; true exactly once per hold, when the two have been held LOCK_HOLD_MS with nothing else down
bool lock_combo_tick(lock_combo_t *c, uint32_t now);
// 0-100 how far the hold has got (the lock screen's bar), 0 when not armed
int  lock_combo_percent(lock_combo_t const *c, uint32_t now);
// "fw keys send" while locked (#112): Esc and Backspace (LVGL key codes 27 and 8) go down and stay down, as a held
// combo, so a script can test the real unlock; any other key is dropped and lets go of everything. true when the key
// was one of the two.
bool lock_combo_probe_key(lock_combo_t *c, uint32_t lv_key, uint32_t now);
