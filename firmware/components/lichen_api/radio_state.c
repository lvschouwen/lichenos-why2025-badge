#include "radio_state.h"

void radio_state_init(radio_state_t *s) {
    s->on       = RADIO_MASK_ALL;
    s->saved    = RADIO_MASK_ALL;
    s->airplane = false;
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

bool radio_state_get(radio_state_t const *s, radio_id_t id) {
    return (unsigned)id < RADIO_COUNT && (s->on >> id & 1u);
}

uint8_t radio_state_set(radio_state_t *s, radio_id_t id, bool on) {
    if ((unsigned)id >= RADIO_COUNT) {
        return 0;
    }
    uint8_t before = s->on;
    s->on          = on ? (uint8_t)(s->on | (1u << id)) : (uint8_t)(s->on & ~(1u << id));
    return before ^ s->on;
}

uint8_t radio_state_set_airplane(radio_state_t *s, bool airplane) {
    if (airplane == s->airplane) {
        return 0;
    }
    uint8_t before = s->on;
    if (airplane) {
        s->saved = s->on;
        s->on    = 0;
    } else {
        s->on = (uint8_t)(s->saved | s->on);
    }
    s->airplane = airplane;
    return before ^ s->on;
}
