// Repeater status history (#168): see mesh_rhist.h
#include "mesh_rhist.h"

#include <stdio.h>
#include <string.h>

uint16_t const RH_INTERVAL_CHOICES[RH_INTERVALS] = {10, 30, 60, 180};

void rh_init(rh_t *t) {
    memset(t, 0, sizeof(*t));
    t->asking = -1;
}

int rh_find(rh_t const *t, uint8_t const prefix[RH_PREFIX]) {
    for (int i = 0; i < RH_SLOTS; i++) {
        if (t->slot[i].used && memcmp(t->slot[i].prefix, prefix, RH_PREFIX) == 0) {
            return i;
        }
    }
    return -1;
}

size_t rh_on_count(rh_t const *t) {
    size_t n = 0;
    for (int i = 0; i < RH_SLOTS; i++) {
        n += t->slot[i].used && t->slot[i].on;
    }
    return n;
}

bool rh_interval_ok(uint16_t minutes) {
    for (int i = 0; i < RH_INTERVALS; i++) {
        if (RH_INTERVAL_CHOICES[i] == minutes) {
            return true;
        }
    }
    return false;
}

uint16_t rh_interval_next(uint16_t minutes) {
    for (int i = 0; i < RH_INTERVALS; i++) {
        if (RH_INTERVAL_CHOICES[i] > minutes) {
            return RH_INTERVAL_CHOICES[i];
        }
    }
    return RH_INTERVAL_CHOICES[0];
}

uint32_t rh_wait_s(rh_slot_t const *s) {
    uint32_t m = s->misses > RH_BACKOFF_MAX ? RH_BACKOFF_MAX : s->misses;
    return ((uint32_t)s->interval_min * 60u) << m;
}

// A free slot, else the switched-off one answered longest ago; -1 when all are on
static int take_slot(rh_t *t) {
    int best = -1;
    for (int i = 0; i < RH_SLOTS; i++) {
        rh_slot_t const *s = &t->slot[i];
        if (!s->used) {
            return i;
        }
        if (!s->on && (best < 0 || s->answer_s < t->slot[best].answer_s)) {
            best = i;
        }
    }
    return best;
}

// The earliest a repeater may be asked: never sooner than one interval after its last ask (a quick off and on does
// not make an extra ask)
static uint32_t earliest_s(rh_slot_t const *s, uint32_t want_s) {
    uint32_t after = s->ask_s ? s->ask_s + (uint32_t)s->interval_min * 60u : 0;
    return want_s > after ? want_s : after;
}

rh_result_t rh_set(rh_t *t, uint8_t const prefix[RH_PREFIX], bool on, uint16_t interval_min, uint32_t now_s) {
    int idx = rh_find(t, prefix);
    if (!on) {
        if (idx >= 0) {
            t->slot[idx].on = false;
            if (t->asking == idx) {
                t->asking = -1; // its answer, if one still comes, is not taken in: it was switched off
            }
        }
        return RH_OK;
    }
    if (!rh_interval_ok(interval_min)) {
        return RH_BAD;
    }
    if (idx < 0) {
        idx = take_slot(t);
        if (idx < 0) {
            return RH_FULL;
        }
        if (t->asking == idx) {
            t->asking = -1;
        }
        memset(&t->slot[idx], 0, sizeof(t->slot[idx]));
        t->slot[idx].used = true;
        memcpy(t->slot[idx].prefix, prefix, RH_PREFIX);
    }
    rh_slot_t *s   = &t->slot[idx];
    bool       was = s->on;
    s->interval_min = interval_min;
    s->on           = true;
    if (!was) {
        s->misses = 0;
        s->next_s = earliest_s(s, now_s + RH_SOON_S);
    } else if (s->ask_s) {
        s->next_s = s->ask_s + rh_wait_s(s); // a new interval counts from the last ask
    }
    return RH_OK;
}

void rh_forget(rh_t *t, uint8_t const prefix[RH_PREFIX]) {
    int idx = rh_find(t, prefix);
    if (idx >= 0) {
        memset(&t->slot[idx], 0, sizeof(t->slot[idx]));
        if (t->asking == idx) {
            t->asking = -1;
        }
    }
}

void rh_load(rh_t *t, uint8_t const prefix[][RH_PREFIX], uint16_t const *interval_min, size_t n, uint32_t now_s) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (rh_set(t, prefix[i], true, interval_min[i], now_s) != RH_OK) {
            continue;
        }
        int idx = rh_find(t, prefix[i]);
        t->slot[idx].next_s = now_s + RH_FIRST_S + (uint32_t)k++ * RH_STAGGER_S; // no burst at a start
    }
}

int rh_due(rh_t const *t, uint32_t now_s, bool busy, uint32_t other_age_s) {
    if (busy || t->asking >= 0 || other_age_s < RH_QUIET_S) {
        return -1;
    }
    if (t->last_ask_s && now_s - t->last_ask_s < RH_GAP_S) {
        return -1;
    }
    int best = -1;
    for (int i = 0; i < RH_SLOTS; i++) {
        rh_slot_t const *s = &t->slot[i];
        if (s->used && s->on && s->next_s <= now_s && (best < 0 || s->next_s < t->slot[best].next_s)) {
            best = i;
        }
    }
    return best;
}

void rh_asked(rh_t *t, int idx, uint32_t seq, uint32_t now_s) {
    rh_slot_t *s  = &t->slot[idx];
    t->asking     = (int8_t)idx;
    t->seq        = seq;
    t->last_ask_s = now_s;
    s->ask_s      = now_s;
    s->asks++;
    s->next_s = now_s + rh_wait_s(s); // also when the end is never seen: the schedule cannot run away
}

void rh_not_started(rh_t *t, int idx, char const *why, uint32_t now_s) {
    t->slot[idx].why    = why;
    t->slot[idx].next_s = now_s + RH_RETRY_S; // not a miss: nothing went out
}

static void record(rh_slot_t *s, mf_status_t const *st, uint32_t now_s) {
    rh_sample_t *x = &s->s[s->head];
    x->at_s        = now_s;
    x->up_secs     = st->up_secs;
    x->n_recv      = st->n_recv;
    x->n_sent      = st->n_sent;
    x->batt_mv     = st->batt_mv;
    x->noise       = st->noise_floor;
    s->head        = (uint16_t)((s->head + 1) % RH_SAMPLES);
    if (s->count < RH_SAMPLES) {
        s->count++;
    }
    s->answers++;
    s->answer_s = now_s;
    s->misses   = 0;
    s->why      = NULL;
}

static bool answered(rr_head_t const *h, uint8_t const prefix[RH_PREFIX]) {
    return h->op == RR_STATUS && h->state == RR_DONE && h->have_status &&
           memcmp(h->status_prefix, prefix, RH_PREFIX) == 0;
}

static bool observe_ours(rh_t *t, rr_head_t const *h, uint32_t now_s) {
    rh_slot_t *s = &t->slot[t->asking];
    if (!h || h->seq != t->seq) {
        t->asking = -1; // MeshCore went away, or our end was not seen before the next request: no miss counted
        s->next_s = s->ask_s + rh_wait_s(s);
        return true;
    }
    if (answered(h, s->prefix)) {
        record(s, &h->status, now_s);
        t->seen_seq = h->seq;
    } else if (h->state == RR_FAILED || h->state == RR_TIMEOUT || h->state == RR_DONE) {
        if (s->misses < 255) {
            s->misses++;
        }
        s->why = h->why ? h->why : "no answer";
    } else {
        return false; // still running
    }
    t->asking = -1;
    s->next_s = s->ask_s + rh_wait_s(s);
    return true;
}

bool rh_observe(rh_t *t, rr_head_t const *h, uint32_t now_s) {
    if (t->asking >= 0) {
        return observe_ours(t, h, now_s);
    }
    if (!h || !h->seq || h->seq == t->seen_seq || h->op != RR_STATUS || h->state != RR_DONE || !h->have_status) {
        return false;
    }
    t->seen_seq = h->seq;
    int idx     = rh_find(t, h->status_prefix);
    if (idx < 0 || !t->slot[idx].on) {
        return false;
    }
    rh_slot_t *s = &t->slot[idx];
    record(s, &h->status, now_s); // the person asked: a real answer, and one ask less on air
    uint32_t next = now_s + rh_wait_s(s);
    s->next_s     = next > s->next_s ? next : s->next_s;
    return true;
}

uint32_t rh_other_age_s(rh_t const *t, rr_head_t const *h) {
    if (!h || !h->seq || h->seq == t->seq) {
        return UINT32_MAX;
    }
    return h->age_ms / 1000u;
}

// ---- the graphs ------------------------------------------------------------------------------------------------------

size_t rh_samples(rh_slot_t const *s, rh_sample_t *out, size_t max) {
    size_t n = s->count < max ? s->count : max;
    for (size_t i = 0; i < n; i++) {
        size_t from = (s->head + RH_SAMPLES - n + i) % RH_SAMPLES; // the newest n, oldest first
        out[i]      = s->s[from];
    }
    return n;
}

static int32_t rate(rh_sample_t const *a, rh_sample_t const *b, bool recv) {
    uint32_t ca = recv ? a->n_recv : a->n_sent, cb = recv ? b->n_recv : b->n_sent;
    uint32_t dt = b->at_s - a->at_s;
    if (dt == 0 || cb < ca || b->up_secs < a->up_secs) {
        return RH_NONE; // the repeater restarted in between: its counters started again from 0
    }
    return (int32_t)(((uint64_t)(cb - ca) * 3600u + dt / 2) / dt);
}

void rh_series(rh_sample_t const *smp, size_t n, rh_field_t f, int32_t *out) {
    for (size_t i = 0; i < n; i++) {
        rh_sample_t const *x = &smp[i];
        switch (f) {
        case RH_BATTERY: out[i] = x->batt_mv ? (int32_t)x->batt_mv : RH_NONE; break;
        case RH_NOISE: out[i] = x->noise; break;
        case RH_RECV_RATE:
        case RH_SENT_RATE: out[i] = i ? rate(&smp[i - 1], x, f == RH_RECV_RATE) : RH_NONE; break;
        case RH_UPTIME: out[i] = (int32_t)(x->up_secs / 3600u); break;
        default: out[i] = RH_NONE; break;
        }
    }
}

bool rh_range(int32_t const *v, size_t n, rh_field_t f, int32_t *lo, int32_t *hi) {
    bool    any = false;
    int32_t a = 0, b = 0;
    for (size_t i = 0; i < n; i++) {
        if (v[i] == RH_NONE) {
            continue;
        }
        a   = any && a < v[i] ? a : v[i];
        b   = any && b > v[i] ? b : v[i];
        any = true;
    }
    if (!any) {
        return false;
    }
    if (f == RH_BATTERY || f == RH_NOISE) {
        int32_t span = f == RH_BATTERY ? 100 : 10;
        if (b - a < span) {
            int32_t grow = span - (b - a);
            a -= grow / 2;
            b = a + span;
        }
    } else {
        a = 0; // counts and hours: from 0, so a flat line is not read as a swing
        b = b < 10 ? 10 : b;
    }
    *lo = a;
    *hi = b;
    return true;
}

// ---- text --------------------------------------------------------------------------------------------------------------

void rh_interval_text(uint16_t minutes, char *out, size_t cap) {
    if (minutes % 60 == 0) {
        snprintf(out, cap, "%u h", (unsigned)(minutes / 60));
    } else {
        snprintf(out, cap, "%u min", (unsigned)minutes);
    }
}

static void wait_text(uint32_t secs, char *out, size_t cap) {
    uint32_t min = (secs + 59) / 60;
    if (min < 60) {
        snprintf(out, cap, "in %lu min", (unsigned long)min);
    } else if (min % 60) {
        snprintf(out, cap, "in %lu h %lu min", (unsigned long)(min / 60), (unsigned long)(min % 60));
    } else {
        snprintf(out, cap, "in %lu h", (unsigned long)(min / 60));
    }
}

void rh_next_text(rh_slot_t const *s, bool asking, uint32_t now_s, char *out, size_t cap) {
    if (!s || !s->used || !s->on) {
        snprintf(out, cap, "Off");
    } else if (asking) {
        snprintf(out, cap, "Asking...");
    } else if (s->next_s <= now_s) {
        snprintf(out, cap, "now");
    } else {
        wait_text(s->next_s - now_s, out, cap);
    }
}

void rh_value_text(rh_field_t f, int32_t v, char *out, size_t cap) {
    if (v == RH_NONE) {
        snprintf(out, cap, "-");
        return;
    }
    switch (f) {
    case RH_BATTERY: snprintf(out, cap, "%ld.%02ld V", (long)(v / 1000), (long)(v % 1000 / 10)); break;
    case RH_NOISE: snprintf(out, cap, "%ld dBm", (long)v); break;
    case RH_RECV_RATE:
    case RH_SENT_RATE: snprintf(out, cap, "%ld/h", (long)v); break;
    case RH_UPTIME: snprintf(out, cap, "%ld h", (long)v); break;
    default: snprintf(out, cap, "%ld", (long)v); break;
    }
}

// ---- the setting -------------------------------------------------------------------------------------------------------

void rh_cfg_encode(rh_t const *t, char *out, size_t cap) {
    size_t at = 0;
    if (!cap) {
        return;
    }
    out[0] = 0;
    for (int i = 0; i < RH_SLOTS; i++) {
        rh_slot_t const *s = &t->slot[i];
        if (!s->used || !s->on) {
            continue;
        }
        uint8_t const *p = s->prefix;
        int w = snprintf(out + at, cap - at, "%s%02x%02x%02x%02x%02x%02x:%u", at ? "," : "", p[0], p[1], p[2], p[3],
                         p[4], p[5], (unsigned)s->interval_min);
        if (w < 0 || (size_t)w >= cap - at) {
            out[at] = 0; // an entry that does not fit is left out whole
            return;
        }
        at += (size_t)w;
    }
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// One "a1b2c3d4e5f6:30" of len bytes; false when it is not one
static bool parse_entry(char const *e, size_t len, uint8_t prefix[RH_PREFIX], uint16_t *minutes) {
    if (len < 2 * RH_PREFIX + 2 || len > 2 * RH_PREFIX + 4 || e[2 * RH_PREFIX] != ':') {
        return false;
    }
    for (int i = 0; i < RH_PREFIX; i++) {
        int a = hexval(e[2 * i]), b = hexval(e[2 * i + 1]);
        if (a < 0 || b < 0) {
            return false;
        }
        prefix[i] = (uint8_t)(a << 4 | b);
    }
    unsigned m = 0;
    for (size_t i = 2 * RH_PREFIX + 1; i < len; i++) {
        if (e[i] < '0' || e[i] > '9') {
            return false;
        }
        m = m * 10 + (unsigned)(e[i] - '0');
    }
    *minutes = (uint16_t)m;
    return rh_interval_ok(*minutes);
}

size_t rh_cfg_decode(char const *text, uint8_t prefix[][RH_PREFIX], uint16_t *interval_min, size_t max) {
    size_t n = 0;
    while (text && *text && n < max) {
        char const *end = strchr(text, ',');
        size_t      len = end ? (size_t)(end - text) : strlen(text);
        if (parse_entry(text, len, prefix[n], &interval_min[n])) {
            bool dup = false;
            for (size_t k = 0; k < n; k++) {
                dup = dup || memcmp(prefix[k], prefix[n], RH_PREFIX) == 0;
            }
            n += !dup;
        }
        text = end ? end + 1 : NULL;
    }
    return n;
}
