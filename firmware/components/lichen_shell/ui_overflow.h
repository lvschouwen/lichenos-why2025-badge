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

// ---- vertical and overlap checks (#133, decision 2026-11-04) -----------------------------------------------------------
// The width check above missed a status label that sat under the pinned hint: the page was taller than the screen.
// These decide the vertical cases from plain numbers; ui_probe.c reads the geometry from LVGL.

typedef struct {
    int32_t x1, y1, x2, y2; // inclusive, as lv_area_t
} ovf_rect_t;

// "No limit" for a span: used when an object has no ancestor on that side of the scroller
#define OVF_NO_LIMIT ((ovf_span_t){-0x40000000, 0x3fffffff})

// A scrollable page can move its content, so a row hidden now is not lost if scrolling shows it. The object sits in
// content that scrolls by up px further up (content moves up: the page's scroll_bottom) and by down px further down
// (scroll_top). inner is the intersection of the object's ancestors below the scroller (they move with the content),
// outer the scroller and everything above it (they stay put). All y spans (x1 = top, x2 = bottom), inclusive.
// With no scroller pass up = down = 0 and put every ancestor in inner (outer = OVF_NO_LIMIT).

// Rows of the object that no scroll position shows (fewest over the positions 0, +up, -down). 0 = fully reachable.
int32_t ovf_hidden_rows(ovf_span_t obj, ovf_span_t inner, ovf_span_t outer, int32_t up, int32_t down);

// Rows of the object's visible part that lie inside `cover` (the pinned hint or the status bar) when both overlap
// in x as well; the fewest over the scroll positions, so content that scrolls clear of the cover is not a hit.
int32_t ovf_cover_rows(ovf_rect_t obj, ovf_span_t inner, ovf_span_t outer, ovf_rect_t cover, int32_t up, int32_t down);

// Rows of wrapped text that do not fit a label of fixed height (text_h at the label's width, box_h its content height).
int32_t ovf_wrap_lost_px(int32_t text_h, int32_t box_h);

// Rows of a label's line box that may be cut without cutting the text: a font's line height carries leading above and
// below the glyphs (~13% each for Lichen's fonts), so a "K" centred in a box a little shorter than its line shows whole.
int32_t ovf_line_slack_px(int32_t line_h);
