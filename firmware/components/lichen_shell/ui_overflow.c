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
