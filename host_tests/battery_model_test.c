#include "battery_model.h"

#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void curve_endpoints_and_interpolation(void) {
    CHECK(bat_mv_to_percent(4200) == 100);
    CHECK(bat_mv_to_percent(3300) == 0);
    CHECK(bat_mv_to_percent(3840) == 50);
    CHECK(bat_mv_to_percent(3825) == 46);  // between 3820 (45) and 3840 (50): 45 + 5*5/20 = 46.25
    CHECK(bat_mv_to_percent(3425) == 3);   // between 3300 (0) and 3550 (5): 125/250*5 = 2.5 rounds to 3
    int prev = 0;
    for (int mv = 3300; mv <= 4200; mv++) {  // monotonic
        int p = bat_mv_to_percent(mv);
        CHECK(p >= prev);
        prev = p;
    }
}

static void curve_clamps(void) {
    CHECK(bat_mv_to_percent(5000) == 100);
    CHECK(bat_mv_to_percent(4201) == 100);
    CHECK(bat_mv_to_percent(3299) == 0);
    CHECK(bat_mv_to_percent(0) == 0);
    CHECK(bat_mv_to_percent(-1) == 0);
    CHECK(bat_mv_to_percent(INT32_MIN) == 0);
    CHECK(bat_mv_to_percent(INT32_MAX) == 100);
}

static void warn_needs_dwell(void) {
    bat_state_t st; bat_reset(&st);
    CHECK(bat_step(&st, 3400, false, 0) == BAT_OK);
    CHECK(bat_step(&st, 3400, false, BAT_DWELL_MS - 1) == BAT_OK);
    CHECK(bat_step(&st, 3400, false, BAT_DWELL_MS) == BAT_WARN);
}

static void one_noisy_sample_changes_nothing(void) {
    bat_state_t st; bat_reset(&st);
    uint32_t t = 0;
    for (int i = 0; i < 20; i++, t += 1000) CHECK(bat_step(&st, 3800, false, t) == BAT_OK);
    CHECK(bat_step(&st, 3000, false, t) == BAT_OK);  // glitch far under the off limit
    t += 1000;
    for (int i = 0; i < 20; i++, t += 1000) CHECK(bat_step(&st, 3800, false, t) == BAT_OK);
}

static void warn_clears_only_above_hysteresis_after_dwell(void) {
    bat_state_t st; bat_reset(&st);
    bat_step(&st, 3390, false, 0);
    CHECK(bat_step(&st, 3390, false, BAT_DWELL_MS) == BAT_WARN);
    uint32_t t = BAT_DWELL_MS;
    for (int i = 0; i < 30; i++) { t += 1000; CHECK(bat_step(&st, 3450, false, t) == BAT_WARN); }  // inside the band
    for (int i = 0; i < 30; i++) { t += 1000; CHECK(bat_step(&st, 3500, false, t) == BAT_WARN); }  // not above 3500
    CHECK(bat_step(&st, 3501, false, t + 1000) == BAT_WARN);                                       // dwell starts
    CHECK(bat_step(&st, 3501, false, t + 1000 + BAT_DWELL_MS) == BAT_OK);
}

static void off_after_dwell_and_latches(void) {
    bat_state_t st; bat_reset(&st);
    bat_step(&st, 3300, false, 0);
    CHECK(bat_step(&st, 3300, false, BAT_DWELL_MS - 1) == BAT_OK);  // neither dwell is done yet
    CHECK(bat_step(&st, 3300, false, BAT_DWELL_MS) == BAT_OFF);
    CHECK(bat_step(&st, 4100, false, BAT_DWELL_MS + 100000) == BAT_OFF);  // latched
}

static void gap_or_recovery_restarts_dwell(void) {
    bat_state_t st; bat_reset(&st);
    bat_step(&st, 3350, false, 0);
    bat_step(&st, 3350, false, 9000);
    CHECK(bat_step(&st, -1, false, 9500) == BAT_OK);        // no reading
    CHECK(bat_step(&st, 3350, false, 10000) == BAT_OK);     // run restarts here
    CHECK(bat_step(&st, 3350, false, 19999) == BAT_OK);
    CHECK(bat_step(&st, 3350, false, 20000) == BAT_WARN);
    bat_state_t c; bat_reset(&c);
    bat_step(&c, 3200, true, 0);                            // charging: never counts
    CHECK(bat_step(&c, 3200, true, 60000) == BAT_OK);
}

static void clock_wrap(void) {
    bat_state_t st; bat_reset(&st);
    uint32_t t0 = 0xFFFFFFFFu - 4000u;
    bat_step(&st, 3350, false, t0);
    CHECK(bat_step(&st, 3350, false, t0 + 9999u) == BAT_OK);
    CHECK(bat_step(&st, 3350, false, t0 + 10000u) == BAT_WARN);
}

static void max17048_decode(void) {
    CHECK(bat_vcell_mv(0xD000) == 4160);   // 53248 * 78.125 uV
    CHECK(bat_vcell_mv(0xC000) == 3840);
    CHECK(bat_vcell_mv(0) == 0);
    CHECK(bat_vcell_mv(0xFFFF) == 5119);
    CHECK(bat_be16(0xD0, 0x00) == 0xD000);
    CHECK(bat_soc_pct256(0x6400) == 0x6400);
    CHECK(bat_soc_percent(0x6400) == 100);
    CHECK(bat_soc_percent(0x5080) == 81);  // 80.5 % rounds up
    CHECK(bat_soc_percent(0x507F) == 80);
    CHECK(bat_soc_percent(0xFFFF) == 100);  // out of range clamps
    CHECK(bat_soc_percent(0) == 0);
    CHECK(bat_crate_centipct_h(0x0001) == 20);
    CHECK(bat_crate_centipct_h(0xFFFF) == -20);
    CHECK(bat_crate_centipct_h(0x7FFF) == 32767 * 104 / 5);
    CHECK(bat_crate_centipct_h(0x8000) == -32768 * 104 / 5);
    CHECK(bat_version_valid(0x0010));
    CHECK(bat_version_valid(0x001F));
    CHECK(!bat_version_valid(0x0000));
    CHECK(!bat_version_valid(0xFFFF));  // empty bus
    CHECK(!bat_version_valid(0x0110));
}

static void trend_guess(void) {
    CHECK(bat_trend_from_crate(0x0005) == BAT_TREND_CHARGING);
    CHECK(bat_trend_from_crate(0x0004) == BAT_TREND_IDLE);
    CHECK(bat_trend_from_crate(0x0000) == BAT_TREND_IDLE);
    CHECK(bat_trend_from_crate(0xFFFC) == BAT_TREND_IDLE);
    CHECK(bat_trend_from_crate(0xFFFB) == BAT_TREND_DISCHARGING);
    CHECK(bat_trend_from_crate(0x8000) == BAT_TREND_DISCHARGING);
}

static void divider_math(void) {
    CHECK(bat_adc_to_cell_mv(2000, 100000, 100000) == 4000);
    CHECK(bat_adc_to_cell_mv(1851, 100000, 100000) == 3702);
    CHECK(bat_adc_to_cell_mv(1000, 200000, 100000) == 3000);
    CHECK(bat_adc_to_cell_mv(1001, 100000, 200000) == 1502);  // 1001*1.5 = 1501.5 rounds up
    CHECK(bat_adc_to_cell_mv(0, 100000, 100000) == 0);
}

static void divider_clamps_and_rejects(void) {
    CHECK(bat_adc_to_cell_mv(3300, 100000, 100000) == BAT_CELL_MAX_MV);  // floating or shorted high pin
    CHECK(bat_adc_to_cell_mv(INT32_MAX, UINT32_MAX, 1) == BAT_CELL_MAX_MV);  // no overflow
    CHECK(bat_adc_to_cell_mv(-5, 100000, 100000) == -1);
    CHECK(bat_adc_to_cell_mv(1000, 100000, 0) == -1);
    CHECK(bat_adc_to_cell_mv(1000, 0, 100000) == -1);
}

int main(void) {
    curve_endpoints_and_interpolation();
    curve_clamps();
    warn_needs_dwell();
    one_noisy_sample_changes_nothing();
    warn_clears_only_above_hysteresis_after_dwell();
    off_after_dwell_and_latches();
    gap_or_recovery_restarts_dwell();
    clock_wrap();
    max17048_decode();
    trend_guess();
    divider_math();
    divider_clamps_and_rejects();
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("battery_model_test ok\n");
    return 0;
}
