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

bool display_swap_proven(uint32_t after_us, uint32_t cb_us, uint32_t guard_us) {
    return (int32_t)(cb_us - after_us) > (int32_t)guard_us;
}
