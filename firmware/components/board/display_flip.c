#include "display_flip.h"

#include <string.h>

unsigned display_back_fb(unsigned front) {
    return front ? 0u : 1u;
}

static int64_t rect_area(display_rect_t r) {
    return (int64_t)(r.x2 - r.x1 + 1) * (r.y2 - r.y1 + 1);
}

static display_rect_t rect_bound(display_rect_t a, display_rect_t b) {
    display_rect_t const u = {
        .x1 = a.x1 < b.x1 ? a.x1 : b.x1,
        .y1 = a.y1 < b.y1 ? a.y1 : b.y1,
        .x2 = a.x2 > b.x2 ? a.x2 : b.x2,
        .y2 = a.y2 > b.y2 ? a.y2 : b.y2,
    };
    return u;
}

static int64_t rect_overlap(display_rect_t a, display_rect_t b) {
    display_rect_t const i = {
        .x1 = a.x1 > b.x1 ? a.x1 : b.x1,
        .y1 = a.y1 > b.y1 ? a.y1 : b.y1,
        .x2 = a.x2 < b.x2 ? a.x2 : b.x2,
        .y2 = a.y2 < b.y2 ? a.y2 : b.y2,
    };
    return i.x2 < i.x1 || i.y2 < i.y1 ? 0 : rect_area(i);
}

// The bounding box of a and b covers no pixel that neither of them covers
static bool rect_tiles(display_rect_t a, display_rect_t b) {
    return rect_area(rect_bound(a, b)) == rect_area(a) + rect_area(b) - rect_overlap(a, b);
}

void display_dirty_clear(display_dirty_t *d) {
    d->n = 0;
}

// Index of the area whose bounding box with r adds the fewest pixels it did not cover before
static uint32_t cheapest_merge(display_dirty_t const *d, display_rect_t r) {
    uint32_t best      = 0;
    int64_t  best_cost = INT64_MAX;
    for (uint32_t i = 0; i < d->n; i++) {
        int64_t const cost = rect_area(rect_bound(d->r[i], r)) - rect_area(d->r[i]);
        if (cost < best_cost) {
            best      = i;
            best_cost = cost;
        }
    }
    return best;
}

void display_dirty_add(display_dirty_t *d, display_rect_t r) {
    if (r.x2 < r.x1 || r.y2 < r.y1) {
        return;
    }
    for (;;) {
        // absorb every area r tiles with; a grown r may tile with one it did not before, so rescan from the start
        uint32_t i = 0;
        while (i < d->n) {
            if (rect_tiles(d->r[i], r)) {
                r       = rect_bound(d->r[i], r);
                d->r[i] = d->r[--d->n];
                i       = 0;
            } else {
                i++;
            }
        }
        if (d->n < DISPLAY_DIRTY_MAX) {
            d->r[d->n++] = r;
            return;
        }
        uint32_t const j = cheapest_merge(d, r);
        r                = rect_bound(d->r[j], r);
        d->r[j]          = d->r[--d->n]; // n < MAX now: the next pass appends
    }
}

void display_copy_rect(uint16_t *dst, uint16_t const *src, int32_t stride_px, display_rect_t r) {
    size_t const bytes = (size_t)(r.x2 - r.x1 + 1) * sizeof(uint16_t);
    for (int32_t y = r.y1; y <= r.y2; y++) {
        size_t const at = (size_t)y * (size_t)stride_px + (size_t)r.x1;
        memcpy(dst + at, src + at, bytes);
    }
}

bool display_swap_proven(display_frame_mark_t req, display_frame_mark_t now, uint32_t guard_us) {
    uint32_t const frames = now.count - req.count;
    if (frames >= 2 && frames < UINT32_MAX / 2) {
        return true;
    }
    return frames == 1 && (int32_t)(now.us - req.us) > (int32_t)guard_us;
}

void display_flip_init(display_flip_t *f, display_flip_ops_t const *ops, bool swap_on) {
    memset(f, 0, sizeof(*f));
    f->ops     = *ops;
    f->swap_on = swap_on;
}

static void note_time(uint64_t *total, uint32_t *max, uint32_t us) {
    *total += us;
    if (us > *max) {
        *max = us;
    }
}

// Bring buffer dst level with src over every area drawn since they were last equal
static void copy_dirty(display_flip_t *f, unsigned dst, unsigned src) {
    uint32_t const t0 = f->ops.now_us(f->ops.ctx);
    for (uint32_t i = 0; i < f->dirty.n; i++) {
        f->ops.copy(f->ops.ctx, dst, src, f->dirty.r[i]);
    }
    display_dirty_clear(&f->dirty);
    note_time(&f->stats.copy_us, &f->stats.copy_max_us, f->ops.now_us(f->ops.ctx) - t0);
}

unsigned display_flip_write_target(display_flip_t *f) {
    if (!f->swap_on) {
        return f->front;
    }
    if (f->pending) {
        f->pending         = false;
        uint32_t const t0  = f->ops.now_us(f->ops.ctx);
        bool const     yes = f->ops.wait(f->ops.ctx, f->req);
        note_time(&f->stats.wait_us, &f->stats.wait_max_us, f->ops.now_us(f->ops.ctx) - t0);
        if (yes) {
            f->stats.swaps++;
            f->front = display_back_fb(f->front);
            copy_dirty(f, display_back_fb(f->front), f->front);
        } else {
            f->stats.timeouts++;
        }
    }
    return display_back_fb(f->front);
}

void display_flip_drawn(display_flip_t *f, display_rect_t r) {
    if (f->swap_on) {
        display_dirty_add(&f->dirty, r);
    }
}

void display_flip_frame_end(display_flip_t *f) {
    if (!f->swap_on) {
        return;
    }
    unsigned const back = display_back_fb(f->front);
    if (f->ops.request(f->ops.ctx, back)) {
        // Count first, then the clock: a callback that lands between the two is counted as after the request but
        // stamped before it, so it proves nothing (the safe side)
        f->req.count = f->ops.frame_count(f->ops.ctx);
        f->req.us    = f->ops.now_us(f->ops.ctx);
        f->pending   = true;
        return;
    }
    f->stats.refusals++;
    copy_dirty(f, f->front, back);
}
