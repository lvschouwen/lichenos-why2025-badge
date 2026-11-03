// Host test: the Weather app's ring, graph range, air trend and CSV line (#155)
#include "weather_series.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)

static wx_sample_t sample(float t, float h, float p, float g) {
    wx_sample_t s = {{t, h, p, g}};
    return s;
}

static void test_ring(void) {
    wx_sample_t store[4];
    wx_ring_t   r;
    wx_ring_init(&r, store, 4);
    float out[8];
    CHECK(wx_series(&r, WX_TEMP, 0, out, 8) == 0);
    for (int i = 1; i <= 3; i++) {
        wx_sample_t s = sample((float)i, 40, 1000, 100);
        wx_ring_push(&r, &s);
    }
    CHECK(wx_series(&r, WX_TEMP, 0, out, 8) == 3 && out[0] == 1 && out[2] == 3);
    for (int i = 4; i <= 6; i++) { // wraps: 3,4,5,6 remain
        wx_sample_t s = sample((float)i, 40, 1000, 100);
        wx_ring_push(&r, &s);
    }
    CHECK(wx_series(&r, WX_TEMP, 0, out, 8) == 4 && out[0] == 3 && out[3] == 6);
    CHECK(wx_series(&r, WX_TEMP, 0, out, 2) == 2 && out[0] == 5 && out[1] == 6); // keeps the newest
    CHECK(wx_series(&r, WX_TEMP, -2.5f, out, 8) == 4 && NEAR(out[3], 3.5f));      // offset on temperature only
    CHECK(wx_series(&r, WX_HUM, -2.5f, out, 8) == 4 && NEAR(out[0], 40));
    wx_sample_t gap = sample(NAN, NAN, NAN, NAN);
    wx_ring_push(&r, &gap);
    CHECK(wx_series(&r, WX_TEMP, -1, out, 8) == 4 && isnan(out[3]));
}

static void test_range(void) {
    float lo, hi;
    float flat[3] = {21.0f, 21.1f, 21.0f};
    CHECK(wx_range(flat, 3, WX_TEMP, &lo, &hi));
    CHECK(hi - lo >= 2.0f && lo < 21.0f && hi > 21.1f); // noise does not fill the graph
    float wide[4] = {10, NAN, 30, 20};
    CHECK(wx_range(wide, 4, WX_TEMP, &lo, &hi) && lo < 10 && hi > 30);
    float none[2] = {NAN, NAN};
    CHECK(!wx_range(none, 2, WX_TEMP, &lo, &hi));
    CHECK(!wx_range(none, 0, WX_HUM, &lo, &hi));
    float gas[2] = {200, 210};
    CHECK(wx_range(gas, 2, WX_GAS, &lo, &hi) && hi - lo >= 80.0f); // relative to the level
    CHECK(wx_chart_value(21.54f) == 215 && wx_chart_value(-0.04f) == 0);
}

static void fill(float *g, size_t n, float v) {
    for (size_t i = 0; i < n; i++) {
        g[i] = v;
    }
}

static void test_air(void) {
    float g[60];
    fill(g, 10, 100);
    CHECK(wx_air_trend(g, 10) == WX_AIR_LEARNING);
    fill(g, 60, 100);
    CHECK(wx_air_trend(g, 60) == WX_AIR_SAME);
    fill(g + 55, 5, 130);
    CHECK(wx_air_trend(g, 60) == WX_AIR_BETTER);
    fill(g + 55, 5, 60);
    CHECK(wx_air_trend(g, 60) == WX_AIR_WORSE);
    fill(g, 60, 100);
    fill(g + 55, 5, 105);
    CHECK(wx_air_trend(g, 60) == WX_AIR_SAME);
    fill(g, 60, NAN);
    CHECK(wx_air_trend(g, 60) == WX_AIR_LEARNING); // a stopped sensor is not "cleaner"
    fill(g, 55, 100);
    fill(g + 55, 5, NAN);
    CHECK(wx_air_trend(g, 60) == WX_AIR_LEARNING); // the newest readings are missing
    CHECK(strlen(wx_air_text(WX_AIR_BETTER)) > 0);
}

static void test_text(void) {
    char t[40];
    wx_fmt_value(t, sizeof t, WX_TEMP, 21.46f);
    CHECK(!strcmp(t, "21.5 \xC2\xB0""C"));
    wx_fmt_value(t, sizeof t, WX_HUM, 45.4f);
    CHECK(!strcmp(t, "45%"));
    wx_fmt_value(t, sizeof t, WX_PRESS, 1013.25f);
    CHECK(!strcmp(t, "1013 hPa"));
    wx_fmt_value(t, sizeof t, WX_GAS, 123.4f);
    CHECK(!strcmp(t, "123 kOhm"));
    wx_fmt_value(t, sizeof t, WX_GAS, NAN);
    CHECK(!strcmp(t, "no reading"));
    wx_span_text(t, sizeof t, 0);
    CHECK(!strcmp(t, "now"));
    wx_span_text(t, sizeof t, 46);
    CHECK(!strcmp(t, "45 min"));
    wx_span_text(t, sizeof t, 131);
    CHECK(!strcmp(t, "2 h 10 min"));
    wx_span_text(t, sizeof t, 361);
    CHECK(!strcmp(t, "6 h"));
}

static void test_offset(void) {
    char t[16];
    CHECK(wx_offset_byte(0) == 20 && wx_offset_half_from_byte(20) == 0);
    CHECK(wx_offset_half_from_byte(0) == -20 && wx_offset_half_from_byte(255) == 0); // clamped
    CHECK(wx_offset_byte(-3) == 17 && wx_offset_byte(-99) == 0 && wx_offset_byte(5) == 20);
    wx_offset_text(t, sizeof t, 0);
    CHECK(!strcmp(t, "0 K"));
    wx_offset_text(t, sizeof t, -3);
    CHECK(!strcmp(t, "-1.5 K"));
    wx_offset_text(t, sizeof t, -20);
    CHECK(!strcmp(t, "-10.0 K"));
}

static void test_csv(void) {
    char line[160];
    CHECK(wx_csv_header(line, sizeof line) > 20 && strstr(line, "time_utc"));
    wx_sample_t s = sample(25.0f, 40.5f, 1013.25f, 150.0f);
    size_t      n = wx_csv_line(line, sizeof line, 1767225600, &s, -3.5f); // 2026-01-01T00:00:00Z
    CHECK(n == strlen(line));
    CHECK(!strcmp(line, "2026-01-01T00:00:00Z,21.50,40.5,1013.25,150.0\n"));
    wx_sample_t gap = sample(NAN, NAN, 1000.0f, NAN);
    wx_csv_line(line, sizeof line, 1767225600, &gap, -3.5f);
    CHECK(!strcmp(line, "2026-01-01T00:00:00Z,,,1000.00,\n"));
    CHECK(wx_csv_line(line, 20, 0, &s, 0) == 0); // too small a buffer writes nothing
}

int main(void) {
    test_ring();
    test_range();
    test_air();
    test_text();
    test_offset();
    test_csv();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("weather_series_test: ok\n");
    return 0;
}
