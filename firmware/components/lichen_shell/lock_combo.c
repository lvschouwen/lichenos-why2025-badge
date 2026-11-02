#include "lock_combo.h"

#include <string.h>

#define CODES (sizeof(((lock_combo_t *)0)->down) * 8)

static bool is_down(lock_combo_t const *c, uint8_t code) {
    return c->down[code / 8] & (1u << (code % 8));
}

static bool only_the_two(lock_combo_t const *c) {
    for (unsigned code = 0; code < CODES; code++) {
        if (code != LOCK_KEY_A && code != LOCK_KEY_B && is_down(c, (uint8_t)code)) {
            return false;
        }
    }
    return is_down(c, LOCK_KEY_A) && is_down(c, LOCK_KEY_B);
}

void lock_combo_reset(lock_combo_t *c) {
    memset(c, 0, sizeof(*c));
}

void lock_combo_event(lock_combo_t *c, uint8_t code, bool pressed, uint32_t now) {
    if (code >= CODES) {
        return;
    }
    if (pressed) {
        c->down[code / 8] |= (uint8_t)(1u << (code % 8));
    } else {
        c->down[code / 8] &= (uint8_t)~(1u << (code % 8));
    }
    bool ok = only_the_two(c);
    if (ok && !c->armed && !c->fired) {
        c->since = now;
    }
    c->armed = ok && !c->fired;
    if (!is_down(c, LOCK_KEY_A) && !is_down(c, LOCK_KEY_B)) {
        c->fired = false;
    }
}

bool lock_combo_tick(lock_combo_t *c, uint32_t now) {
    if (!c->armed || now - c->since < LOCK_HOLD_MS) {
        return false;
    }
    c->armed = false;
    c->fired = true;
    return true;
}

int lock_combo_percent(lock_combo_t const *c, uint32_t now) {
    if (!c->armed) {
        return 0;
    }
    uint32_t held = now - c->since;
    return held >= LOCK_HOLD_MS ? 100 : (int)(held * 100 / LOCK_HOLD_MS);
}

bool lock_combo_probe_key(lock_combo_t *c, uint32_t lv_key, uint32_t now) {
    if (lv_key == 27) {
        lock_combo_event(c, LOCK_KEY_A, true, now);
        return true;
    }
    if (lv_key == 8) {
        lock_combo_event(c, LOCK_KEY_B, true, now);
        return true;
    }
    lock_combo_reset(c);
    return false;
}
