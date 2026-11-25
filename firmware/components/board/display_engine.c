#include "display_engine.h"

#include "display_video_geom.h"

#include <string.h>

static char const *const names[DISPLAY_ENGINE_COUNT] = DISPLAY_ENGINE_NAMES;

bool display_engine_valid(uint32_t id) {
    return id < DISPLAY_ENGINE_COUNT;
}

char const *display_engine_name(uint32_t id) {
    return display_engine_valid(id) ? names[id] : "?";
}

bool display_strip_scaled(display_strip_t const *s) {
    return s->scale_x16 != 0 || s->scale_y16 != 0;
}

void display_strip_ppa_params(display_strip_t const *s, display_ppa_params_t *p) {
    uint32_t const w = (uint32_t)(s->ui.x2 - s->ui.x1 + 1), h = (uint32_t)(s->ui.y2 - s->ui.y1 + 1);
    p->pic_w         = s->stride / 2;
    if (!display_strip_scaled(s)) {
        *p = (display_ppa_params_t){.pic_w = p->pic_w, .pic_h = h, .blk_w = w, .blk_h = h, .scale_x = 1.0f, .scale_y = 1.0f};
        return;
    }
    p->pic_h   = s->src_h;
    p->off_x   = (uint32_t)s->crop.x1;
    p->off_y   = (uint32_t)s->crop.y1;
    p->blk_w   = (uint32_t)(s->crop.x2 - s->crop.x1 + 1);
    p->blk_h   = (uint32_t)(s->crop.y2 - s->crop.y1 + 1);
    p->scale_x = (float)s->scale_x16 / DISPLAY_VIDEO_SCALE_STEPS;
    p->scale_y = (float)s->scale_y16 / DISPLAY_VIDEO_SCALE_STEPS;
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
    e->want  = (display_engine_id_t)id;
    e->rearm = false; // a choice made by hand stands
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
    e->cpu_since_us = e->ops.now_us(e->ops.ctx);
    e->ops.fell_back(e->ops.ctx, from, why);
}

static void cpu_turn(display_engine_t *e, display_strip_t const *s, unsigned fb) {
    if (display_strip_scaled(s)) {
        e->refused = true; // the CPU path has no scaler: no half picture
        return;
    }
    uint32_t const t0 = e->ops.now_us(e->ops.ctx);
    e->ops.cpu(e->ops.ctx, s, fb);
    note_rotate(e, e->ops.now_us(e->ops.ctx) - t0);
}

// Two waits (#475): the first bound (a strip takes a few ms) catches a done that a flash write held back; the late one
// keeps the CPU from redoing the strip, or later strips from being drawn, while a slow transfer may still write its
// old strip over them. Done in the late wait: the PPA is idle and fine, so re-arm it after a while (a release build
// has no switch to do it by hand), at most DISPLAY_ENGINE_LATE_MAX times. Done in neither: hung, CPU for the boot.
static display_fallback_t late_outcome(display_engine_t *e, bool late_done) {
    if (!late_done) {
        e->hung     = true; // until its own done turns up (display_engine_ppa_done_seen)
        e->hung_gen = e->gen;
        return DISPLAY_FALLBACK_TIMEOUT;
    }
    e->late_fallbacks++;
    e->rearm = e->late_fallbacks < DISPLAY_ENGINE_LATE_MAX;
    return e->rearm ? DISPLAY_FALLBACK_LATE : DISPLAY_FALLBACK_LATE_FINAL;
}

// At a frame boundary (nothing in flight), as a select applies
static void maybe_rearm(display_engine_t *e) {
    if (!e->rearm || e->hung || e->active != DISPLAY_ENGINE_CPU ||
        e->ops.now_us(e->ops.ctx) - e->cpu_since_us < DISPLAY_ENGINE_REARM_US) {
        return;
    }
    e->rearm = false;
    e->rearms++;
    e->want = DISPLAY_ENGINE_PPA_ASYNC;
    e->ops.fell_back(e->ops.ctx, DISPLAY_ENGINE_CPU, DISPLAY_EVENT_REARM);
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
    fall_back(e, late_outcome(e, e->ops.ppa_wait(e->ops.ctx, &done_us, true)));
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
        maybe_rearm(e);
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

static bool present_strip(display_engine_t *e, display_strip_t const *s, unsigned fb) {
    e->mid_frame = false;
    turn(e, s, fb, true); // synchronous: a present has no LVGL flush_ready to hand over
    return !e->refused;
}

display_present_t display_engine_present(display_engine_t *e, display_flip_t *flip, display_video_owner_t *owner,
                                         display_strip_t const *bands, uint32_t n_bands, display_strip_t const *video) {
    display_engine_settle(e);
    if (e->mid_frame || owner->state != DISPLAY_VIDEO_ACTIVE) {
        return DISPLAY_PRESENT_REFUSED;
    }
    if (!video) {
        return DISPLAY_PRESENT_HELD;
    }
    maybe_rearm(e);
    apply_want(e);
    if (e->active == DISPLAY_ENGINE_CPU && display_strip_scaled(video)) {
        e->refused = true;
        return DISPLAY_PRESENT_REFUSED; // before the write target: nothing is touched
    }
    display_video_plan_t const plan = display_video_plan(owner, n_bands, true);
    e->refused                      = false;
    unsigned const fb               = display_flip_write_target(flip);
    for (uint32_t i = 0; i < plan.take; i++) {
        if (!present_strip(e, &bands[i], fb)) {
            return DISPLAY_PRESENT_REFUSED;
        }
        display_flip_drawn(flip, bands[i].blk);
    }
    if (!present_strip(e, video, fb)) { // a fall-back inside the wait: the frame is not shown
        return DISPLAY_PRESENT_REFUSED;
    }
    display_flip_drawn_nocarry(flip, video->blk);
    if (plan.swap) {
        display_flip_frame_end(flip);
    }
    return DISPLAY_PRESENT_OK;
}
