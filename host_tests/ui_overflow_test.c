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

    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("ui_overflow_test: ok\n");
    return 0;
}
