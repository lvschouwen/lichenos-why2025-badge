#include "thermal_rules.h"

#include <string.h>

th_limits_t th_limits(th_src_t src) {
    if (src == TH_SRC_DIE) {
        return (th_limits_t){.warn_c = 75, .clear_c = 70, .shutdown_c = 90, .rise_c = 20, .rise_floor_c = 60};
    }
    return (th_limits_t){.warn_c = 50, .clear_c = 45, .shutdown_c = 60, .rise_c = 10, .rise_floor_c = 40};
}

bool th_plausible(float c) {
    return c >= TH_PLAUSIBLE_MIN && c <= TH_PLAUSIBLE_MAX; // also false for NaN
}

float th_board_from_bme(float raw_c, bool heater_on) {
    (void)heater_on; // no correction: see thermal_rules.h
    return raw_c;
}

void th_reset(th_state_t *st) {
    memset(st, 0, sizeof(*st));
}

static void hist_clear(th_hist_t *h) {
    h->n = h->head = 0;
}

static void hist_push(th_hist_t *h, uint32_t t, float c) {
    h->c[h->head] = c;
    h->t[h->head] = t;
    h->head       = (uint8_t)((h->head + 1) % TH_SAMPLES);
    if (h->n < TH_SAMPLES) {
        h->n++;
    }
}

// Smallest reading of the window before the newest sample, as a rise to the newest
static float hist_rise(th_hist_t const *h, uint32_t now_s) {
    if (h->n < 2) {
        return 0;
    }
    uint8_t newest = (uint8_t)((h->head + TH_SAMPLES - 1) % TH_SAMPLES);
    float   cur    = h->c[newest];
    float   low    = cur;
    for (uint8_t i = 0; i + 1 < h->n; i++) {
        uint8_t k = (uint8_t)((newest + TH_SAMPLES - 1 - i) % TH_SAMPLES);
        if (now_s - h->t[k] > TH_WINDOW_S) {
            break;
        }
        if (h->c[k] < low) {
            low = h->c[k];
        }
    }
    return cur - low;
}

float th_rise(th_state_t const *st, th_src_t src, uint32_t now_s) {
    return hist_rise(&st->hist[src], now_s);
}

typedef struct {
    bool     any, hot, crit, all_cool;
    th_src_t hot_src, crit_src;
    bool     crit_rise;
    float    hot_c, crit_c;
} verdict_t;

static void judge(th_state_t *st, th_input_t const *in, uint32_t now_s, verdict_t *v) {
    v->all_cool = true;
    for (int s = 0; s < TH_SRC_COUNT; s++) {
        th_hist_t *h = &st->hist[s];
        if (!in->src[s].valid || !th_plausible(in->src[s].c)) {
            if (in->src[s].valid) {
                st->implausible[s]++;
            }
            hist_clear(h); // a gap restarts the rise window
            continue;
        }
        float       c = in->src[s].c;
        th_limits_t l = th_limits((th_src_t)s);
        hist_push(h, now_s, c);
        v->any = true;
        if (c >= l.warn_c && !v->hot) {
            v->hot = true, v->hot_src = (th_src_t)s, v->hot_c = c;
        }
        if (c >= l.clear_c) {
            v->all_cool = false;
        }
        bool rise = c >= l.rise_floor_c && hist_rise(h, now_s) >= l.rise_c;
        if ((c >= l.shutdown_c || rise) && !v->crit) {
            v->crit = true, v->crit_src = (th_src_t)s, v->crit_c = c, v->crit_rise = c < l.shutdown_c;
        }
    }
}

static unsigned blind_step(th_state_t *st, bool any, uint32_t now_s) {
    if (any) {
        st->last_reading_s = now_s;
        if (st->blind) {
            st->blind = false;
            return TH_EV_SIGHT;
        }
    } else if (!st->blind && now_s - st->last_reading_s >= TH_BLIND_S) {
        st->blind = true;
        return TH_EV_BLIND;
    }
    return 0;
}

// The board sensor on its own: lost after TH_BLIND_S without a usable reading, back with the first one
static unsigned board_step(th_state_t *st, th_input_t const *in, uint32_t now_s) {
    th_reading_t r  = in->src[TH_SRC_BOARD];
    bool         ok = r.valid && th_plausible(r.c);
    if (!st->started_board || ok) {
        st->started_board = true; // the first poll starts the clock
        st->board_ok_s    = now_s;
    }
    if (ok && st->board_lost) {
        st->board_lost = false;
        return TH_EV_BOARD_BACK;
    }
    if (!ok && !st->board_lost && now_s - st->board_ok_s >= TH_BLIND_S) {
        st->board_lost = true;
        return TH_EV_BOARD_LOST;
    }
    return 0;
}

unsigned th_step(th_state_t *st, th_input_t const *in, uint32_t now_s) {
    if (!st->started) {
        st->started        = true;
        st->last_reading_s = now_s;
    }
    verdict_t v = {0};
    judge(st, in, now_s, &v);
    unsigned ev = blind_step(st, v.any, now_s) | board_step(st, in, now_s);
    if (st->level == TH_SHUTDOWN) {
        return ev;
    }
    st->crit_polls = v.crit ? (uint8_t)(st->crit_polls + 1) : 0;
    if (st->crit_polls >= TH_CONFIRM) {
        st->level      = TH_SHUTDOWN;
        st->cause      = v.crit_src;
        st->cause_rise = v.crit_rise;
        st->cause_c    = v.crit_c;
        return ev | TH_EV_SHUTDOWN;
    }
    st->hot_polls = v.hot ? (uint8_t)(st->hot_polls + 1) : 0;
    if (st->level == TH_OK && st->hot_polls >= TH_CONFIRM) {
        st->level      = TH_WARN;
        st->cause      = v.hot_src;
        st->cause_rise = false;
        st->cause_c    = v.hot_c;
        st->cool_polls = 0;
        return ev | TH_EV_WARN;
    }
    // No readings at all keep the warning where it is: cooling must be seen
    st->cool_polls = (v.any && v.all_cool) ? (uint8_t)(st->cool_polls + 1) : 0;
    if (st->level == TH_WARN && st->cool_polls >= TH_CONFIRM) {
        st->level = TH_OK;
        return ev | TH_EV_CLEARED;
    }
    return ev;
}

char const *th_src_name(th_src_t src) {
    return src == TH_SRC_DIE ? "P4 die" : "board (BME690)";
}

char const *th_level_name(th_level_t level) {
    return level == TH_SHUTDOWN ? "shutdown" : level == TH_WARN ? "warn" : "ok";
}
