#pragma once

#include <stdbool.h>
#include <stdint.h>

// `fw ui overflow` (#133): the pure decision of whether a label's text is cut off. Host-tested in
// host_tests/ui_overflow_test.c; ui_probe.c feeds it from LVGL.
typedef enum { OVF_WRAP, OVF_DOT, OVF_SCROLL, OVF_CLIP } ovf_mode_t; // SCROLL covers both scroll modes

typedef struct {
    int32_t x1, x2; // inclusive, as lv_area_t
} ovf_span_t;

// The horizontal pixels of the text that are not visible.
//   text_w   the whole text on one line (no width limit), in px
//   label    the label's own span
//   clip     the label's span intersected with every ancestor's and the screen (the visible box)
//   mode     the label's long mode
// What shows: CLIP draws the whole text but cuts it at the label's edge; every other mode keeps the text inside the
// label (a wrapped or dotted label of real width fits by design; one that is content-sized is as wide as its text).
// So the text reaches min(text_w, label width) (CLIP: text_w, cut at the label's edge) from the label's left, and
// the part right of clip.x2 is lost, as is the part of the label left of clip.x1. SCROLL labels scroll their text,
// but their box can still be cut by a parent. Text alignment is ignored (left-aligned assumed). *have_out, if given,
// gets the px available from the label's left edge to the clip's right edge.
int32_t ui_overflow_px(int32_t text_w, ovf_span_t label, ovf_span_t clip, ovf_mode_t mode, int32_t *have_out);
