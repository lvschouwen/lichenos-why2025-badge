#include "ui_overflow.h"
#include <stddef.h>

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

static int32_t rows(ovf_span_t s) {
    return s.x2 >= s.x1 ? s.x2 - s.x1 + 1 : 0;
}

// The object's rows inside outer when the content has moved up by `shift` (base = the object cut by its inner ancestors,
// which move with it)
static int32_t shown_rows(ovf_span_t base, ovf_span_t outer, int32_t shift) {
    return rows((ovf_span_t){imax(base.x1 - shift, outer.x1), imin(base.x2 - shift, outer.x2)});
}

#define MAX_SHIFTS 8

// The scroll positions worth trying. Visible rows and cover rows are piecewise linear in the shift, so the best
// position is an end of the range or one where an edge of the object meets an edge of outer (or of the cover); the
// middle of a long list is reached by those, not by the ends alone (#133 review). All clamped to [-down, up].
static int shifts_for(ovf_span_t base, ovf_span_t outer, ovf_span_t const *cover, int32_t up, int32_t down,
                      int32_t out[MAX_SHIFTS]) {
    int32_t lo = -imax(down, 0), hi = imax(up, 0);
    int32_t raw[MAX_SHIFTS] = {0, hi, lo, base.x1 - outer.x1, base.x2 - outer.x2, 0, 0, 0};
    int     n = 5;
    if (cover) {
        raw[n++] = base.x2 - cover->x1 + 1; // the object's bottom just above the cover
        raw[n++] = base.x1 - cover->x2 - 1; // its top just below the cover
    }
    for (int i = 0; i < n; i++) {
        out[i] = imin(imax(raw[i], lo), hi);
    }
    return n;
}

int32_t ovf_hidden_rows(ovf_span_t obj, ovf_span_t inner, ovf_span_t outer, int32_t up, int32_t down) {
    int32_t height = rows(obj);
    if (height == 0) {
        return 0;
    }
    ovf_span_t base = {imax(obj.x1, inner.x1), imin(obj.x2, inner.x2)};
    int32_t    sh[MAX_SHIFTS];
    int        n    = shifts_for(base, outer, NULL, up, down, sh);
    int32_t    best = 0;
    for (int i = 0; i < n; i++) {
        best = imax(best, shown_rows(base, outer, sh[i]));
    }
    return height - best;
}

int32_t ovf_cover_rows(ovf_rect_t obj, ovf_span_t inner, ovf_span_t outer, ovf_rect_t cover, int32_t up, int32_t down) {
    ovf_span_t col = {obj.y1, obj.y2};
    if (obj.x2 < cover.x1 || obj.x1 > cover.x2 || rows(col) == 0) {
        return 0;
    }
    ovf_span_t base = {imax(col.x1, inner.x1), imin(col.x2, inner.x2)};
    ovf_span_t cv   = {cover.y1, cover.y2};
    int32_t    sh[MAX_SHIFTS];
    int        n = shifts_for(base, outer, &cv, up, down, sh);
    // Only positions that show as much of the object as any position can count: one that scrolls it out of sight is
    // not "clear of the cover"
    int32_t most = 0;
    for (int i = 0; i < n; i++) {
        most = imax(most, shown_rows(base, outer, sh[i]));
    }
    int32_t best = -1;
    for (int i = 0; i < n; i++) {
        if (shown_rows(base, outer, sh[i]) != most) {
            continue;
        }
        ovf_span_t s = {imax(base.x1 - sh[i], outer.x1), imin(base.x2 - sh[i], outer.x2)};
        int32_t    r = rows((ovf_span_t){imax(s.x1, cv.x1), imin(s.x2, cv.x2)});
        best         = best < 0 ? r : imin(best, r);
    }
    return best < 0 ? 0 : best;
}

int32_t ovf_wrap_lost_px(int32_t text_h, int32_t box_h) {
    return text_h > box_h && box_h > 0 ? text_h - box_h : 0;
}

int32_t ovf_line_slack_px(int32_t line_h) {
    return line_h * 3 / 10;
}
