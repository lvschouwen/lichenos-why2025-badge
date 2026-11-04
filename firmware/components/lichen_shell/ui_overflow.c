#include "ui_overflow.h"

int32_t ui_overflow_px(int32_t text_w, ovf_span_t label, ovf_span_t clip, ovf_mode_t mode, int32_t *have_out) {
    int32_t label_w = label.x2 - label.x1 + 1;
    int32_t have    = clip.x2 - label.x1 + 1;
    if (have_out) {
        *have_out = have;
    }
    if (text_w <= 0) {
        return 0;
    }
    int32_t need = text_w;
    if (mode == OVF_CLIP) {
        if (have > label_w) {
            have = label_w; // CLIP cuts at the label's own edge, whatever the parent has room for
        }
    } else if (need > label_w) {
        need = label_w; // WRAP / DOT / SCROLL keep the text inside the label
    }
    int32_t over = need > have ? need - have : 0;
    if (mode == OVF_SCROLL) {
        over = label_w > have ? label_w - have : 0; // the text scrolls; only a cut label box counts
    }
    if (clip.x1 > label.x1) {
        over += clip.x1 - label.x1; // the left edge lies under a parent's border
    }
    return over;
}

static int32_t imax(int32_t a, int32_t b) {
    return a > b ? a : b;
}

static int32_t imin(int32_t a, int32_t b) {
    return a < b ? a : b;
}

// The object's rows left after the clips, with the content moved up by `shift` (inner moves with it, outer stays)
static ovf_span_t shown_span(ovf_span_t obj, ovf_span_t inner, ovf_span_t outer, int32_t shift) {
    ovf_span_t s = {imax(obj.x1 - shift, imax(inner.x1 - shift, outer.x1)),
                    imin(obj.x2 - shift, imin(inner.x2 - shift, outer.x2))};
    return s;
}

static int32_t rows(ovf_span_t s) {
    return s.x2 >= s.x1 ? s.x2 - s.x1 + 1 : 0;
}

int32_t ovf_hidden_rows(ovf_span_t obj, ovf_span_t inner, ovf_span_t outer, int32_t up, int32_t down) {
    int32_t height = rows(obj);
    if (height == 0) {
        return 0;
    }
    int32_t shifts[3] = {0, imax(up, 0), -imax(down, 0)};
    int32_t best      = height;
    for (int i = 0; i < 3; i++) {
        best = imin(best, height - rows(shown_span(obj, inner, outer, shifts[i])));
    }
    return best;
}

int32_t ovf_cover_rows(ovf_rect_t obj, ovf_span_t inner, ovf_span_t outer, ovf_rect_t cover, int32_t up, int32_t down) {
    if (obj.x2 < cover.x1 || obj.x1 > cover.x2 || rows((ovf_span_t){obj.y1, obj.y2}) == 0) {
        return 0;
    }
    int32_t shifts[3] = {0, imax(up, 0), -imax(down, 0)};
    int32_t best      = -1;
    for (int i = 0; i < 3; i++) {
        ovf_span_t s = shown_span((ovf_span_t){obj.y1, obj.y2}, inner, outer, shifts[i]);
        int32_t    n = rows((ovf_span_t){imax(s.x1, cover.y1), imin(s.x2, cover.y2)});
        best         = best < 0 ? n : imin(best, n);
    }
    return best;
}

int32_t ovf_wrap_lost_px(int32_t text_h, int32_t box_h) {
    return text_h > box_h && box_h > 0 ? text_h - box_h : 0;
}
