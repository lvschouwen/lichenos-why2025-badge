// Host tests for the LoRa spectrum app's sums (#161): the sweep plan, the graph scale, the peak hold and the texts
#include "lora_spectrum.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// Every bin lies inside the band, the steps are what was asked, the channel has a bin centred on it
static void check_plan_inside(spec_band_t const *b, spec_plan_t const *p, uint8_t max_bins) {
    CHECK(p->count >= 1 && p->count <= max_bins);
    CHECK(p->first_hz - p->step_hz / 2 >= b->start_hz);
    CHECK(p->first_hz + p->step_hz * (p->count - 1u) + p->step_hz / 2 <= b->end_hz);
}

static void plan_narrow_band_on_the_channel(void) {
    spec_plan_t p;
    CHECK(spec_plan(&spec_bands[0], 869618000, 62500, 128, &p));
    CHECK(p.step_hz == 62500);
    CHECK(p.channel_bin == 25);
    CHECK(p.first_hz + 25u * 62500 == 869618000);
    CHECK(p.count == 31);
    check_plan_inside(&spec_bands[0], &p, 128);
}

static void plan_wide_band(void) {
    spec_plan_t p;
    CHECK(spec_plan(&spec_bands[1], 869618000, 62500, 128, &p));
    CHECK(p.step_hz == 62500 && p.count == 111);
    CHECK(p.channel_bin >= 0 && p.first_hz + (uint32_t)p.channel_bin * 62500 == 869618000);
    check_plan_inside(&spec_bands[1], &p, 128);

    // a narrow channel: the step doubles until the band fits the bins
    CHECK(spec_plan(&spec_bands[1], 869618000, 7800, 128, &p));
    CHECK(p.step_hz == 62400 && p.count <= 128);
    check_plan_inside(&spec_bands[1], &p, 128);
    CHECK(spec_plan(&spec_bands[1], 869618000, 62500, 32, &p));
    CHECK(p.step_hz == 250000 && p.count <= 32);
    check_plan_inside(&spec_bands[1], &p, 32);
}

static void plan_channel_outside(void) {
    spec_plan_t p;
    CHECK(spec_plan(&spec_bands[0], 915000000, 125000, 128, &p));
    CHECK(p.channel_bin == -1);
    CHECK(p.first_hz == 868000000 + 62500 && p.step_hz == 125000 && p.count == 16);
    check_plan_inside(&spec_bands[0], &p, 128);
    // the channel too close to the edge for a whole bin: no bin of its own, the grid starts at the edge
    CHECK(spec_plan(&spec_bands[0], 868010000, 62500, 128, &p));
    CHECK(p.channel_bin == -1 && p.first_hz == 868031250);
}

static void plan_refuses(void) {
    spec_plan_t       p;
    spec_band_t const tiny = {"tiny", 868000000, 868050000};
    CHECK(!spec_plan(&spec_bands[0], 869618000, 0, 128, &p));
    CHECK(!spec_plan(&spec_bands[0], 869618000, 62500, 0, &p));
    CHECK(!spec_plan(&tiny, 868025000, 62500, 128, &p));
}

static void bar_scale(void) {
    CHECK(spec_bar_px(SPEC_FLOOR_DBM * 10, 200) == SPEC_MIN_BAR);
    CHECK(spec_bar_px(-1500, 200) == SPEC_MIN_BAR); // below the floor
    CHECK(spec_bar_px(SPEC_TOP_DBM * 10, 200) == 200);
    CHECK(spec_bar_px(-100, 200) == 200); // above the top
    CHECK(spec_bar_px(-850, 200) == 100); // halfway
    CHECK(spec_bar_px(-1120, 180) == 36);
}

static void bin_spans(void) {
    int x, w;
    spec_bin_span(0, 32, 648, &x, &w);
    CHECK(x == 0 && w == 19); // 20 px less the gap
    spec_bin_span(31, 32, 648, &x, &w);
    CHECK(x + w + 1 == 648);
    int prev_end = 0;
    for (uint8_t i = 0; i < 111; i++) {
        spec_bin_span(i, 111, 648, &x, &w);
        CHECK(x >= prev_end && w >= 1);
        prev_end = x + w;
    }
    CHECK(prev_end <= 648);
    spec_bin_span(0, 0, 648, &x, &w);
    CHECK(x == 0 && w == 0);
}

static void peak_hold(void) {
    int16_t peak[3];
    spec_peak_reset(peak, 3);
    int16_t const a[3] = {-1200, -600, -1100};
    int16_t const b[3] = {-1150, -1180, -1190};
    spec_peak_update(peak, a, 3);
    spec_peak_update(peak, b, 3);
    CHECK(peak[0] == -1150 && peak[1] == -600 && peak[2] == -1100);
    CHECK(spec_strongest(peak, 3) == 1);
    CHECK(spec_strongest(b, 3) == 0);
    CHECK(spec_strongest(b, 0) == 0);
}

static void texts(void) {
    char t[32];
    spec_fmt_mhz(869618000, t, sizeof(t));
    CHECK(strcmp(t, "869.618 MHz") == 0);
    spec_fmt_mhz(868031250, t, sizeof(t));
    CHECK(strcmp(t, "868.031 MHz") == 0);
    spec_fmt_mhz(869999600, t, sizeof(t));
    CHECK(strcmp(t, "870.000 MHz") == 0);
    spec_fmt_dbm(-1125, t, sizeof(t));
    CHECK(strcmp(t, "-112.5 dBm") == 0);
    spec_fmt_dbm(-5, t, sizeof(t));
    CHECK(strcmp(t, "-0.5 dBm") == 0);
    spec_fmt_dbm(30, t, sizeof(t));
    CHECK(strcmp(t, "3.0 dBm") == 0);
    spec_fmt_khz(62500, t, sizeof(t));
    CHECK(strcmp(t, "62.5 kHz") == 0);
    spec_fmt_khz(125000, t, sizeof(t));
    CHECK(strcmp(t, "125 kHz") == 0);
}

int main(void) {
    plan_narrow_band_on_the_channel();
    plan_wide_band();
    plan_channel_outside();
    plan_refuses();
    bar_scale();
    bin_spans();
    peak_hold();
    texts();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("lora_spectrum_test: all passed\n");
    return 0;
}
