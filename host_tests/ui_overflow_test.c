// Host test: the `fw ui overflow` decision (#133)
#include "ui_overflow.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static int32_t px(int32_t text, int32_t l1, int32_t l2, int32_t c1, int32_t c2, ovf_mode_t m) {
    return ui_overflow_px(text, (ovf_span_t){l1, l2}, (ovf_span_t){c1, c2}, m, NULL);
}

// ---- vertical and overlap checks ----------------------------------------------------------------------------------
static ovf_span_t span(int32_t a, int32_t b) {
    return (ovf_span_t){a, b};
}

static int32_t cover(int32_t y1, int32_t y2, ovf_rect_t hint, int32_t up, int32_t down) {
    return ovf_cover_rows((ovf_rect_t){36, y1, 683, y2}, OVF_NO_LIMIT, span(0, 719), hint, up, down);
}

static void test_vertical(void) {
    // The LoRa spectrum regression (#161, fixed in c5f01612). Hand-computed from open_spectrum's flex column with
    // 24 px body text of ~32 px lines and the 40 px heading of ~54 px: screen pad_top 92 (56 bar + 36), heading 62
    // (54 + 8 pad_bottom), caption 64, graph GRAPH_H, axis row 32, channel / now / highest 32 each, status 64, with 8 px
    // between all of them. The hint strip is 8 (gap) + 32 (line) + 18 (edge) = 58 px: y 662..719.
    // GRAPH_H 200: status label y 602..665, its bottom (665) is below the hint's top (662). GRAPH_H 160: 562..625.
    ovf_rect_t hint = {0, 662, 719, 719};
    CHECK(cover(602, 665, hint, 0, 0) == 4);
    CHECK(cover(602, 665, hint, 3, 0) == 1); // the screen's pad lets it scroll 3 px: still under the hint
    CHECK(cover(562, 625, hint, 0, 0) == 0); // GRAPH_H 160 clears it
    CHECK(cover(661, 661, hint, 0, 0) == 0); // touching is not overlapping
    CHECK(cover(662, 662, hint, 0, 0) == 1);
    // a page that scrolls clear of the hint is not a hit; one that cannot is
    CHECK(cover(600, 700, hint, 200, 0) == 0);
    CHECK(cover(600, 700, hint, 20, 0) == 19);  // shifted up 20: 580..680 leaves 662..680
    // an object right of the cover does not overlap it
    CHECK(ovf_cover_rows((ovf_rect_t){800, 650, 900, 700}, OVF_NO_LIMIT, span(0, 719), (ovf_rect_t){0, 662, 719, 719},
                         0, 0) == 0);
    // the part hidden by a clipping ancestor does not count: a row cut at 640 by its parent never reaches the hint
    CHECK(ovf_cover_rows((ovf_rect_t){36, 600, 683, 700}, span(0, 640), span(0, 719), hint, 0, 0) == 0);

    // clipped by the screen: y beyond 719
    CHECK(ovf_hidden_rows(span(700, 759), OVF_NO_LIMIT, span(0, 719), 0, 0) == 40);
    CHECK(ovf_hidden_rows(span(720, 779), OVF_NO_LIMIT, span(0, 719), 0, 0) == 60); // wholly off
    CHECK(ovf_hidden_rows(span(600, 700), OVF_NO_LIMIT, span(0, 719), 0, 0) == 0);
    // above the top
    CHECK(ovf_hidden_rows(span(-10, 20), OVF_NO_LIMIT, span(0, 719), 0, 0) == 10);
    // content that scrolls into view is reachable; a short scroll is not enough
    CHECK(ovf_hidden_rows(span(700, 759), OVF_NO_LIMIT, span(0, 719), 100, 0) == 0);
    CHECK(ovf_hidden_rows(span(700, 759), OVF_NO_LIMIT, span(0, 719), 10, 0) == 30);
    CHECK(ovf_hidden_rows(span(-10, 20), OVF_NO_LIMIT, span(0, 719), 0, 50) == 0);
    // an ancestor below the scroller clips with its content: a row beyond a non-scrolling box stays hidden
    CHECK(ovf_hidden_rows(span(300, 399), span(0, 349), span(0, 719), 500, 500) == 50);
    CHECK(ovf_hidden_rows(span(10, 20), OVF_NO_LIMIT, span(0, 719), 0, 0) == 0);

    // a row in the middle of a long scrolling list (#133 review): reachable, though neither end of the scroll shows it
    CHECK(ovf_hidden_rows(span(3000, 3059), OVF_NO_LIMIT, span(0, 719), 5000, 0) == 0);
    CHECK(ovf_hidden_rows(span(3000, 3059), OVF_NO_LIMIT, span(0, 719), 5000, 5000) == 0);
    CHECK(ovf_hidden_rows(span(-3000, -2941), OVF_NO_LIMIT, span(0, 719), 0, 5000) == 0);
    CHECK(ovf_hidden_rows(span(3000, 3059), OVF_NO_LIMIT, span(0, 719), 2000, 0) == 60); // not far enough
    CHECK(ovf_hidden_rows(span(3000, 3059), OVF_NO_LIMIT, span(0, 719), 2300, 0) == 40); // 700..759: 20 rows
    CHECK(ovf_hidden_rows(span(3000, 3059), OVF_NO_LIMIT, span(0, 719), 2400, 0) == 0);
    // under the hint: clear only at a middle scroll (the end of the range scrolls it out of sight, which is no cure)
    CHECK(cover(640, 699, hint, 700, 0) == 0);
    CHECK(cover(640, 699, hint, 20, 0) == 18); // cannot get clear: best is 20 up, 662..679
    CHECK(cover(640, 699, hint, 38, 0) == 0);

    // wrapped text taller than the label's fixed height
    CHECK(ovf_wrap_lost_px(96, 64) == 32);
    CHECK(ovf_wrap_lost_px(64, 64) == 0);
    CHECK(ovf_wrap_lost_px(32, 64) == 0);
    CHECK(ovf_wrap_lost_px(96, 0) == 0);
    // a glyph centred in a box a little shorter than its line (home's "K" icon: 54 px line, 14 px cut) is not a hit
    CHECK(ovf_line_slack_px(54) >= 14);
    CHECK(ovf_line_slack_px(54) < 54);
}

int main(void) {
    // content-sized label (as wide as its text) grown past its parent: the classic default label
    CHECK(px(300, 100, 399, 0, 299, OVF_WRAP) == 100);
    CHECK(px(300, 100, 399, 0, 299, OVF_DOT) == 100);
    CHECK(px(300, 100, 399, 0, 299, OVF_CLIP) == 100);
    // fits
    CHECK(px(200, 100, 299, 0, 299, OVF_DOT) == 0);
    // real width that fits the box: dotted or wrapped long text is fine
    CHECK(px(900, 10, 209, 0, 299, OVF_DOT) == 0);
    CHECK(px(900, 10, 209, 0, 299, OVF_WRAP) == 0);
    // CLIP with real width cuts the text even though the parent has room
    CHECK(px(300, 10, 209, 0, 299, OVF_CLIP) == 100);
    // real-width label wider than the parent, text short: nothing visible is lost
    CHECK(px(50, 10, 499, 0, 299, OVF_DOT) == 0);
    // real-width label wider than the parent, text long: cut by the parent
    CHECK(px(900, 10, 499, 0, 299, OVF_DOT) == 200);
    // scroll labels: only a cut box counts
    CHECK(px(900, 10, 209, 0, 299, OVF_SCROLL) == 0);
    CHECK(px(900, 10, 399, 0, 299, OVF_SCROLL) == 100);
    // left edge under the parent's border
    CHECK(px(50, -20, 29, 0, 299, OVF_DOT) == 20);
    // have is reported
    int32_t have = 0;
    ui_overflow_px(300, (ovf_span_t){100, 399}, (ovf_span_t){0, 299}, OVF_WRAP, &have);
    CHECK(have == 200);
    CHECK(px(0, 100, 399, 0, 299, OVF_WRAP) == 0);

    test_vertical();

    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("ui_overflow_test: ok\n");
    return 0;
}
