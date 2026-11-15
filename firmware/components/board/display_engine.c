#include "display_engine.h"

#include <string.h>

static char const *const names[DISPLAY_ENGINE_COUNT] = DISPLAY_ENGINE_NAMES;

bool display_engine_valid(uint32_t id) {
    return id < DISPLAY_ENGINE_COUNT;
}

char const *display_engine_name(uint32_t id) {
    return display_engine_valid(id) ? names[id] : "?";
}

void display_engine_init(display_engine_t *e, display_engine_ops_t const *ops) {
    memset(e, 0, sizeof(*e));
    e->ops    = *ops;
    e->active = DISPLAY_ENGINE_CPU;
    e->want   = DISPLAY_ENGINE_CPU;
}

static void apply_want(display_engine_t *e) {
    if (e->active != e->want) {
        e->active = e->want;
        memset(&e->stats, 0, sizeof(e->stats)); // one engine per stats line
    }
}

uint32_t display_engine_gen_next(uint32_t g) {
    return g + 1 ? g + 1 : 1;
}

bool display_engine_done_is_current(uint32_t current, uint32_t got) {
    return got != 0 && got == current;
}

void display_engine_ppa_done_seen(display_engine_t *e, uint32_t got) {
    if (e->hung && got == e->hung_gen) {
        e->hung = false;
    }
}

bool display_engine_ppa_hung(display_engine_t const *e) {
    return e->hung;
}

bool display_engine_select(display_engine_t *e, uint32_t id) {
    if (!display_engine_valid(id) || (id != DISPLAY_ENGINE_CPU && e->hung)) {
        return false;
    }
    e->want = (display_engine_id_t)id;
    if (!e->mid_frame && !e->in_flight) {
        e->active = DISPLAY_ENGINE_COUNT; // force the stats reset even when re-selecting the same engine
        apply_want(e);
    }
    return true;
}

display_engine_id_t display_engine_active(display_engine_t const *e) {
    return e->active;
}

static void note_rotate(display_engine_t *e, uint32_t us) {
    e->stats.strips++;
    e->stats.rotate_us += us;
    if (us > e->stats.rotate_max_us) {
        e->stats.rotate_max_us = us;
    }
}

static void fall_back(display_engine_t *e, display_fallback_t why) {
    display_engine_id_t const from = e->active;
    e->active = e->want = DISPLAY_ENGINE_CPU;
    e->stats.fallbacks++;
    e->ops.fell_back(e->ops.ctx, from, why);
}

static void cpu_turn(display_engine_t *e, display_strip_t const *s, unsigned fb) {
    uint32_t const t0 = e->ops.now_us(e->ops.ctx);
    e->ops.cpu(e->ops.ctx, s, fb);
    note_rotate(e, e->ops.now_us(e->ops.ctx) - t0);
}

void display_engine_settle(display_engine_t *e) {
    if (!e->in_flight) {
        return;
    }
    e->in_flight     = false;
    uint32_t done_us = 0;
    if (e->ops.ppa_wait(e->ops.ctx, &done_us, false)) {
        note_rotate(e, done_us - e->start_us);
        return;
    }
    if (!e->ops.ppa_wait(e->ops.ctx, &done_us, true)) {
        e->hung     = true; // until its own done turns up (display_engine_ppa_done_seen)
        e->hung_gen = e->gen;
    }
    fall_back(e, DISPLAY_FALLBACK_TIMEOUT);
    cpu_turn(e, &e->flight, e->flight_fb); // the PPA may have written nothing: the frame stays right
}

// True = the strip is in the buffer (or the last strip's transfer is done); false = in flight, the interrupt readies
static bool turn(display_engine_t *e, display_strip_t const *s, unsigned fb, bool last) {
    if (e->active != DISPLAY_ENGINE_CPU) {
        bool const     background = !last;
        uint32_t const t0         = e->ops.now_us(e->ops.ctx);
        e->gen                    = display_engine_gen_next(e->gen);
        if (e->ops.ppa_start(e->ops.ctx, s, fb, background, e->gen)) {
            e->in_flight = true;
            e->start_us  = t0;
            e->flight    = *s;
            e->flight_fb = fb;
            if (background) {
                return false;
            }
            display_engine_settle(e); // a timeout redoes the strip on the CPU
            return true;
        }
        fall_back(e, DISPLAY_FALLBACK_START);
    }
    cpu_turn(e, s, fb);
    return true;
}

bool display_engine_flush(display_engine_t *e, display_flip_t *flip, display_strip_t const *s, bool last) {
    display_engine_settle(e); // normally done already: LVGL waited for it before handing this strip over
    if (!e->mid_frame) {
        apply_want(e);
    }
    e->mid_frame      = !last;
    unsigned const fb = display_flip_write_target(flip); // may copy back: nothing is in flight here
    bool const     ok = turn(e, s, fb, last);
    display_flip_drawn(flip, s->blk);
    if (last) {
        display_flip_frame_end(flip); // the last strip's transfer is done: turn() waited for it
    }
    return ok;
}
