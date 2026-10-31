#include "hold_exit.h"

void hold_exit_event(hold_exit_t *h, bool is_esc, bool pressed, uint32_t now) {
    if (!is_esc) {
        return;
    }
    if (pressed && !h->down) {
        h->down  = true;
        h->fired = false;
        h->since = now;
    } else if (!pressed) {
        h->down = false;
    }
}

bool hold_exit_tick(hold_exit_t *h, uint32_t now) {
    if (!h->down || h->fired || (uint32_t)(now - h->since) < HOLD_EXIT_MS) {
        return false;
    }
    h->fired = true;
    return true;
}

int hold_exit_percent(hold_exit_t const *h, uint32_t now) {
    if (!h->down) {
        return 0;
    }
    uint32_t held = now - h->since;
    return held >= HOLD_EXIT_MS ? 100 : (int)(held * 100 / HOLD_EXIT_MS);
}
