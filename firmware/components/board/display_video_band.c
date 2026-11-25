#include "display_video_band.h"

#include <string.h>

void display_band_q_init(display_band_q_t *q, uint32_t cap_bytes) {
    memset(q, 0, sizeof(*q));
    q->cap = cap_bytes;
}

uint32_t display_band_bytes(display_rect_t r) {
    uint32_t const w = (uint32_t)(r.x2 - r.x1 + 1), h = (uint32_t)(r.y2 - r.y1 + 1);
    uint32_t const raw = w * 2 * h;
    return (raw + DISPLAY_BAND_ALIGN - 1) / DISPLAY_BAND_ALIGN * DISPLAY_BAND_ALIGN;
}

static bool same_rect(display_rect_t a, display_rect_t b) {
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

static void note_lost(display_band_q_t *q, display_rect_t r) {
    if (!q->lost) {
        q->lost_rect = r;
    } else {
        q->lost_rect.x1 = r.x1 < q->lost_rect.x1 ? r.x1 : q->lost_rect.x1;
        q->lost_rect.y1 = r.y1 < q->lost_rect.y1 ? r.y1 : q->lost_rect.y1;
        q->lost_rect.x2 = r.x2 > q->lost_rect.x2 ? r.x2 : q->lost_rect.x2;
        q->lost_rect.y2 = r.y2 > q->lost_rect.y2 ? r.y2 : q->lost_rect.y2;
    }
    q->lost = true;
    q->overflows++;
}

display_band_t const *display_band_q_put(display_band_q_t *q, display_rect_t r) {
    for (uint32_t i = 0; i < q->n; i++) {
        if (same_rect(q->e[i].ui, r)) {
            display_band_t const keep = q->e[i];
            memmove(&q->e[i], &q->e[i + 1], (q->n - i - 1) * sizeof(q->e[0]));
            q->e[q->n - 1] = keep;
            return &q->e[q->n - 1];
        }
    }
    uint32_t const need = display_band_bytes(r);
    if (q->n >= DISPLAY_BAND_MAX || q->used + need > q->cap) {
        note_lost(q, r);
        return NULL;
    }
    q->e[q->n] = (display_band_t){.ui = r, .off = q->used};
    q->used += need;
    return &q->e[q->n++];
}

uint32_t display_band_q_count(display_band_q_t const *q) {
    return q->n;
}

void display_band_q_pop_all(display_band_q_t *q) {
    q->n    = 0;
    q->used = 0;
}

bool display_band_q_take_lost(display_band_q_t *q, display_rect_t *r) {
    if (!q->lost) {
        return false;
    }
    *r      = q->lost_rect;
    q->lost = false;
    return true;
}

void display_band_q_clear(display_band_q_t *q) {
    display_band_q_pop_all(q);
    q->lost = false;
}
