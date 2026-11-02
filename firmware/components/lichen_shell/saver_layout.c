#include "saver_layout.h"

// Bits: a=0 b=1 c=2 d=3 e=4 f=5 g=6
static uint8_t const DIGITS[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

uint8_t seg7_mask(char c) {
    if (c >= '0' && c <= '9') {
        return DIGITS[c - '0'];
    }
    return c == '-' ? 0x40 : 0;
}

seg_rect_t seg7_rect(int seg, int32_t w, int32_t h, int32_t t) {
    int32_t m    = (h - t) / 2; // top of the middle bar
    int32_t vert = m - t;       // a vertical bar between two horizontal ones
    switch (seg) {
        case 0: return (seg_rect_t){t, 0, w - 2 * t, t};
        case 1: return (seg_rect_t){w - t, t, t, vert};
        case 2: return (seg_rect_t){w - t, m + t, t, h - t - (m + t)};
        case 3: return (seg_rect_t){t, h - t, w - 2 * t, t};
        case 4: return (seg_rect_t){0, m + t, t, h - t - (m + t)};
        case 5: return (seg_rect_t){0, t, t, vert};
        case 6: return (seg_rect_t){t, m, w - 2 * t, t};
        default: return (seg_rect_t){0, 0, 0, 0};
    }
}

// A triangle wave through 0 at t = 0: up to span, down to -span, back, period 4 * span
static int32_t bounce(uint32_t t, int32_t span) {
    if (span <= 0) {
        return 0;
    }
    uint32_t period = 4u * (uint32_t)span;
    int32_t  p      = (int32_t)((t + (uint32_t)span) % period);
    return p < 2 * span ? p - span : 3 * span - p;
}

// Steps per minute in pixels, different per axis (and coprime-ish to the spans) so the path does not retrace itself
#define DRIFT_X 23u
#define DRIFT_Y 17u

void saver_drift(uint32_t step, int32_t span_x, int32_t span_y, int32_t *x, int32_t *y) {
    *x = bounce(step * DRIFT_X, span_x);
    *y = bounce(step * DRIFT_Y, span_y);
}
