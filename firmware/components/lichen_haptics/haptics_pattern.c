#include "haptics_pattern.h"

static hp_step_t const TICK[]   = {{40, 0}};
static hp_step_t const NOTIFY[] = {{80, 80}, {80, 0}};
static hp_step_t const ALERT[]  = {{300, 100}, {300, 0}};

hp_step_t const *hp_steps(hp_pattern_t p, size_t *count) {
    switch (p) {
    case HP_NOTIFY:
        *count = sizeof(NOTIFY) / sizeof(NOTIFY[0]);
        return NOTIFY;
    case HP_ALERT:
        *count = sizeof(ALERT) / sizeof(ALERT[0]);
        return ALERT;
    case HP_TICK:
    default:
        *count = sizeof(TICK) / sizeof(TICK[0]);
        return TICK;
    }
}

static uint32_t scaled(hp_strength_t s, uint32_t ms) {
    uint32_t pct = s == HP_LOW ? 75 : s == HP_MEDIUM ? 100 : 125;
    return ms * pct / 100;
}

uint32_t hp_total_ms(hp_pattern_t p, hp_strength_t s) {
    size_t           n;
    hp_step_t const *st    = hp_steps(p, &n);
    uint32_t         total = 0;
    for (size_t i = 0; i < n; i++) {
        total += scaled(s, st[i].on_ms) + (i + 1 < n ? st[i].off_ms : 0);
    }
    return total;
}

uint8_t hp_hold_duty(hp_strength_t s) {
    switch (s) {
    case HP_LOW:
        return 230; // 90 %
    case HP_MEDIUM:
        return 242; // 95 %
    case HP_HIGH:
    default:
        return 255;
    }
}

uint8_t hp_duty_at(hp_pattern_t p, hp_strength_t s, uint32_t t_ms) {
    size_t           n;
    hp_step_t const *st = hp_steps(p, &n);
    for (size_t i = 0; i < n; i++) {
        uint32_t on = scaled(s, st[i].on_ms);
        if (t_ms < on) {
            return t_ms < HP_KICK_MS ? 255 : hp_hold_duty(s); // every pulse starts with the kick
        }
        t_ms -= on;
        if (t_ms < st[i].off_ms) {
            return 0;
        }
        t_ms -= st[i].off_ms;
    }
    return 0;
}

bool hp_done(hp_pattern_t p, hp_strength_t s, uint32_t t_ms) {
    return t_ms >= hp_total_ms(p, s);
}

bool hp_allowed(bool enabled, bool updating) {
    return enabled && !updating;
}

hp_pattern_t hp_for_notice(uint8_t level) {
    return level >= 2 ? HP_ALERT : HP_NOTIFY;
}

bool hp_gate_take(hp_gate_t *g, uint32_t now_ms, uint32_t min_gap_ms) {
    if (g->any && (uint32_t)(now_ms - g->last_ms) < min_gap_ms) {
        return false;
    }
    g->any     = true;
    g->last_ms = now_ms;
    return true;
}

uint8_t hp_duty_pct(uint32_t duty8) {
    if (duty8 > 255) {
        duty8 = 255;
    }
    return (uint8_t)((duty8 * 100 + 127) / 255);
}
