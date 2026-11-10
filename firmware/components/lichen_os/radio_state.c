#include "radio_state.h"

#include <string.h>

void radio_state_init(radio_state_t *s) {
    memset(s, 0, sizeof(*s)); // no holds: they never outlive a boot (#282)
    s->on    = RADIO_MASK_DEFAULT;
    s->saved = RADIO_MASK_DEFAULT;
}

void radio_state_load(radio_state_t *s, uint8_t on, uint8_t saved, uint8_t airplane) {
    radio_state_init(s);
    if (on > RADIO_MASK_ALL || saved > RADIO_MASK_ALL || airplane > 1) {
        return;
    }
    s->on       = on;
    s->saved    = saved;
    s->airplane = airplane == 1;
}

uint8_t radio_state_migrate(radio_state_t *s, uint8_t version) {
    if (version < 1) {
        s->on    = (uint8_t)(s->on & ~(1u << RADIO_BT));
        s->saved = (uint8_t)(s->saved & ~(1u << RADIO_BT));
    }
    return RADIO_STATE_VERSION;
}

static uint8_t held_mask(radio_state_t const *s) {
    uint8_t m = 0;
    for (int r = 0; r < RADIO_HOLD_COUNT; r++) {
        m |= s->held[r];
    }
    return m;
}

uint8_t radio_state_effective(radio_state_t const *s) {
    return (uint8_t)(s->on & ~held_mask(s));
}

bool radio_state_get(radio_state_t const *s, radio_id_t id) {
    return (unsigned)id < RADIO_COUNT && (radio_state_effective(s) >> id & 1u);
}

uint8_t radio_state_set(radio_state_t *s, radio_id_t id, bool on) {
    if ((unsigned)id >= RADIO_COUNT) {
        return 0;
    }
    uint8_t before = radio_state_effective(s);
    uint8_t bit    = (uint8_t)(1u << id);
    if (on) {
        s->on |= bit;
        for (int r = 0; r < RADIO_HOLD_COUNT; r++) { // the person switched it on: that overrides a hold
            s->held[r] &= (uint8_t)~bit;
        }
    } else {
        s->on &= (uint8_t)~bit;
    }
    return before ^ radio_state_effective(s);
}

uint8_t radio_state_set_airplane(radio_state_t *s, bool airplane) {
    if (airplane == s->airplane) {
        return 0;
    }
    uint8_t before = radio_state_effective(s);
    if (airplane) {
        s->saved = s->on;
        s->on    = 0;
    } else {
        s->on = (uint8_t)(s->saved | s->on);
    }
    s->airplane = airplane;
    return before ^ radio_state_effective(s);
}

static bool hold_reason_ok(radio_hold_t reason) {
    return reason > RADIO_HOLD_NONE && reason < RADIO_HOLD_COUNT;
}

uint8_t radio_state_hold(radio_state_t *s, radio_hold_t reason, uint8_t mask) {
    if (!hold_reason_ok(reason)) {
        return 0;
    }
    uint8_t before = radio_state_effective(s);
    s->held[reason] |= (uint8_t)(mask & RADIO_MASK_ALL);
    return before ^ radio_state_effective(s);
}

uint8_t radio_state_release(radio_state_t *s, radio_hold_t reason) {
    if (!hold_reason_ok(reason)) {
        return 0;
    }
    uint8_t before  = radio_state_effective(s);
    s->held[reason] = 0;
    return before ^ radio_state_effective(s);
}

radio_hold_t radio_state_held_by(radio_state_t const *s, radio_id_t id) {
    if ((unsigned)id >= RADIO_COUNT) {
        return RADIO_HOLD_NONE;
    }
    for (int r = RADIO_HOLD_NONE + 1; r < RADIO_HOLD_COUNT; r++) {
        if (s->held[r] >> id & 1u) {
            return (radio_hold_t)r;
        }
    }
    return RADIO_HOLD_NONE;
}
