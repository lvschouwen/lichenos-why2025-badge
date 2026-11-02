#include "station_clock.h"

#include <math.h>

#define FULL 3600 // tenths of a degree

int32_t sc_second_angle(uint32_t ms) {
    if (ms >= SC_SWEEP_MS) {
        return 0; // waiting at 12 for the minute
    }
    return (int32_t)((uint64_t)ms * FULL / SC_SWEEP_MS);
}

int32_t sc_minute_angle(int min) {
    return ((min % 60 + 60) % 60) * 60;
}

int32_t sc_hour_angle(int hour, int min) {
    return ((hour % 12 + 12) % 12) * 300 + ((min % 60 + 60) % 60) * 5;
}

// Proportions after the classic station clock: long hour marks, a hand that reaches the minute track, a disc on the
// second hand at about two thirds of the radius
sc_dims_t sc_dims(int32_t r) {
    int32_t outer = r * 95 / 100;
    return (sc_dims_t){
        .hour_mark   = {r * 72 / 100, outer, r / 12},
        .minute_mark = {r * 86 / 100, outer, r / 36 > 2 ? r / 36 : 2},
        .hour        = {-r * 24 / 100, r * 64 / 100, r / 9},
        .minute      = {-r * 24 / 100, r * 92 / 100, r / 13},
        .second      = {-r * 31 / 100, r * 62 / 100, r / 45 > 2 ? r / 45 : 2},
        .disc_r      = r * 105 / 1000,
        .cap_r       = r / 40 > 2 ? r / 40 : 2,
    };
}

static void unit(int32_t a, float *sx, float *cy) {
    float rad = (float)a * (float)M_PI / (FULL / 2);
    *sx       = sinf(rad);
    *cy       = cosf(rad);
}

sc_point_t sc_polar(int32_t cx, int32_t cy, int32_t a, int32_t r) {
    float s, c;
    unit(a, &s, &c);
    return (sc_point_t){cx + (int32_t)lroundf((float)r * s), cy - (int32_t)lroundf((float)r * c)};
}

void sc_bar_ends(int32_t cx, int32_t cy, int32_t a, sc_bar_t bar, sc_point_t *p0, sc_point_t *p1) {
    *p0 = sc_polar(cx, cy, a, bar.r0);
    *p1 = sc_polar(cx, cy, a, bar.r1);
}

// The rotated rectangle's four corners, then their extremes; floor/ceil so the box never cuts a pixel the bar touches
sc_box_t sc_bar_box(int32_t cx, int32_t cy, int32_t a, int32_t w, int32_t r0, int32_t r1, int32_t pad) {
    float s, c;
    unit(a, &s, &c);
    float hw = (float)w / 2;
    float px = c * hw, py = s * hw; // half the width, at right angles to the bar
    float xs[4], ys[4];
    for (int i = 0; i < 4; i++) {
        float r   = (float)(i < 2 ? r0 : r1);
        float sgn = (i & 1) ? -1.0f : 1.0f;
        xs[i]     = (float)cx + r * s + sgn * px;
        ys[i]     = (float)cy - r * c + sgn * py;
    }
    float x1 = xs[0], x2 = xs[0], y1 = ys[0], y2 = ys[0];
    for (int i = 1; i < 4; i++) {
        x1 = fminf(x1, xs[i]);
        x2 = fmaxf(x2, xs[i]);
        y1 = fminf(y1, ys[i]);
        y2 = fmaxf(y2, ys[i]);
    }
    sc_box_t b = {(int32_t)floorf(x1), (int32_t)floorf(y1), (int32_t)ceilf(x2), (int32_t)ceilf(y2)};
    return (sc_box_t){b.x1 - pad, b.y1 - pad, b.x2 + pad, b.y2 + pad};
}

sc_box_t sc_disc_box(int32_t cx, int32_t cy, int32_t a, int32_t r, int32_t dr, int32_t pad) {
    sc_point_t p = sc_polar(cx, cy, a, r);
    return (sc_box_t){p.x - dr - pad, p.y - dr - pad, p.x + dr + pad, p.y + dr + pad};
}

#define LAYOUT_EDGE 8 // free above the dial and below the last line
#define LAYOUT_GAP  8 // dial to the first line

sc_layout_t sc_layout(int32_t w, int32_t h, int32_t line1_h, int32_t line2_h) {
    int32_t d = h - 2 * LAYOUT_EDGE - LAYOUT_GAP - line1_h - line2_h;
    if (d > w) {
        d = w;
    }
    d &= ~1;
    if (d < SC_MIN_DIAL) {
        return (sc_layout_t){0};
    }
    int32_t y1 = LAYOUT_EDGE + d + LAYOUT_GAP;
    return (sc_layout_t){d, LAYOUT_EDGE, y1, y1 + line1_h};
}
