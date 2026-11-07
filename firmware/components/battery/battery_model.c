#include "battery_model.h"

typedef struct { int16_t mv; uint8_t pct; } curve_pt_t;
// Descending in voltage; see the source note in battery_model.h.
static const curve_pt_t curve[] = {
    {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75}, {3950, 70}, {3910, 65},
    {3870, 60},  {3850, 55}, {3840, 50}, {3820, 45}, {3800, 40}, {3790, 35}, {3770, 30}, {3730, 25},
    {3700, 20},  {3680, 15}, {3650, 10}, {3550, 5},  {3300, 0},
};
#define CURVE_N (sizeof curve / sizeof curve[0])

uint8_t bat_mv_to_percent(int32_t mv) {
    if (mv >= curve[0].mv) return 100;
    if (mv <= curve[CURVE_N - 1].mv) return 0;
    for (unsigned i = 1; i < CURVE_N; i++) {
        if (mv >= curve[i].mv) {
            int32_t span = curve[i - 1].mv - curve[i].mv;
            int32_t rise = curve[i - 1].pct - curve[i].pct;
            return (uint8_t)(curve[i].pct + ((mv - curve[i].mv) * rise + span / 2) / span);
        }
    }
    return 0;
}

void bat_reset(bat_state_t *st) { *st = (bat_state_t){0}; }

// Times a condition: starts on the first true sample, reports true once it has held for BAT_DWELL_MS.
static bool held(bool cond, bool *active, uint32_t *since, uint32_t now) {
    if (!cond) { *active = false; return false; }
    if (!*active) { *active = true; *since = now; }
    return (uint32_t)(now - *since) >= BAT_DWELL_MS;
}

bat_level_t bat_step(bat_state_t *st, int32_t mv, bool charging, uint32_t now) {
    bool have = mv >= 0;
    bool off  = held(have && !charging && mv <= BAT_OFF_MV, &st->off_active, &st->off_since_ms, now);
    bool low  = held(have && !charging && mv <= BAT_WARN_MV, &st->low_active, &st->low_since_ms, now);
    bool ok   = held(have && mv > BAT_WARN_CLEAR_MV, &st->ok_active, &st->ok_since_ms, now);
    if (st->level == BAT_OFF) return BAT_OFF;  // latched
    if (off) st->level = BAT_OFF;
    else if (low) st->level = BAT_WARN;
    else if (ok) st->level = BAT_OK;
    return st->level;
}

uint32_t bat_vcell_mv(uint16_t raw) { return (uint32_t)raw * 5u / 64u; }

uint16_t bat_soc_pct256(uint16_t raw) { return raw > 100u * 256u ? 100u * 256u : raw; }

uint8_t bat_soc_percent(uint16_t raw) { return (uint8_t)((bat_soc_pct256(raw) + 128u) >> 8); }

int32_t bat_crate_centipct_h(uint16_t raw) {
    int32_t s = (int16_t)raw;
    return s * 104 / 5;  // 0.208 %/h = 20.8 centi-percent/h per LSB
}

bool bat_version_valid(uint16_t raw) { return (raw & 0xFFF0u) == 0x0010u; }

bat_trend_t bat_trend_from_crate(uint16_t raw) {
    int32_t c = bat_crate_centipct_h(raw);
    if (c >= BAT_TREND_MIN_CENTIPCT) return BAT_TREND_CHARGING;
    if (c <= -BAT_TREND_MIN_CENTIPCT) return BAT_TREND_DISCHARGING;
    return BAT_TREND_IDLE;
}

int32_t bat_adc_to_cell_mv(int32_t adc_mv, uint32_t r_top, uint32_t r_bot) {
    if (adc_mv < 0 || r_bot == 0 || r_top == 0) return -1;
    int64_t mv = ((int64_t)adc_mv * ((int64_t)r_top + r_bot) + r_bot / 2) / r_bot;
    return mv > BAT_CELL_MAX_MV ? BAT_CELL_MAX_MV : (int32_t)mv;
}
