#pragma once
// Battery model (#11). Pure (numbers in, numbers out), host-tested in host_tests/battery_model_test.c. The drivers
// (MAX17048 on I2C2 0x36, optional ADC divider on GPIO19) only read registers/millivolts and hand them to this file.
#include <stdbool.h>
#include <stdint.h>

// ---- Li-ion voltage to percent -------------------------------------------------------------------------------
// Open-circuit curve of a generic 4.20 V Li-ion/18650 cell, interpolated linearly between table points. The points
// are the commonly published rest-voltage figures for such cells (e.g. Battery University "BU-501"-style discharge
// tables, and the table used by the Adafruit/SparkFun LiPo guides); they are typical, not measured on this badge's
// cells, and under load the voltage sags, so this is a guess. The MAX17048 SOC (ModelGauge) is better when present.
// Out-of-range input clamps: at or above 4200 mV is 100 %, at or below 3300 mV is 0 %.
uint8_t bat_mv_to_percent(int32_t cell_mv);

// ---- Warning and power-off thresholds ------------------------------------------------------------------------
#define BAT_WARN_MV      3400   // warn at or below this
#define BAT_WARN_CLEAR_MV 3500  // the warning clears only above this (100 mV hysteresis)
#define BAT_OFF_MV       3300   // power off at or below this
#define BAT_DWELL_MS     10000  // a condition must hold this long, continuously, before it counts

typedef enum { BAT_OK = 0, BAT_WARN, BAT_OFF } bat_level_t;

typedef struct {
    bat_level_t level;       // OFF latches (the badge powers off); BAT_OK/BAT_WARN move with the voltage
    bool        low_active;  // a below-warn run is being timed
    uint32_t    low_since_ms;
    bool        off_active;  // a below-off run is being timed
    uint32_t    off_since_ms;
    bool        ok_active;   // an above-clear run is being timed
    uint32_t    ok_since_ms;
} bat_state_t;

void bat_reset(bat_state_t *st);
// One sample at now_ms (wrapping uint32 ms clock). cell_mv < 0 means "no reading": it restarts every dwell, so a gap
// never counts as time spent low. charging true restarts the low/off dwells too (a charger lifts the reading, and a
// battery that charges is not about to die). Returns the level after this sample. A noisy single sample cannot change
// the level because every transition needs BAT_DWELL_MS of consistent samples.
bat_level_t bat_step(bat_state_t *st, int32_t cell_mv, bool charging, uint32_t now_ms);

// ---- MAX17048 registers (Analog Devices/Maxim datasheet "MAX17048/MAX17049", register map) -------------------------
#define MAX17048_ADDR       0x36
#define MAX17048_REG_VCELL  0x02  // 16 bit, 78.125 uV per LSB (so 0xD000 = 4.16 V)
#define MAX17048_REG_SOC    0x04  // 16 bit, high byte 1 %, low byte 1/256 %
#define MAX17048_REG_VERSION 0x08 // 16 bit, production version 0x001_ (low nibble varies)
#define MAX17048_REG_CRATE  0x16  // signed 16 bit, 0.208 %/hour per LSB

// Registers arrive most significant byte first on I2C.
static inline uint16_t bat_be16(uint8_t hi, uint8_t lo) { return (uint16_t)((hi << 8) | lo); }

uint32_t bat_vcell_mv(uint16_t raw);        // millivolts, truncated: raw * 78.125 uV = raw * 5 / 64 mV
uint16_t bat_soc_pct256(uint16_t raw);      // percent in 1/256 steps (the raw value), clamped to 100.0 %
uint8_t  bat_soc_percent(uint16_t raw);     // whole percent, rounded, 0..100
int32_t  bat_crate_centipct_h(uint16_t raw); // charge rate in 0.01 %/hour (raw as int16 * 0.208 %/h), + = charging
bool     bat_version_valid(uint16_t raw);   // looks like a MAX17048 (0x001_); anything else is another chip at 0x36

typedef enum { BAT_TREND_UNKNOWN = 0, BAT_TREND_CHARGING, BAT_TREND_DISCHARGING, BAT_TREND_IDLE } bat_trend_t;
// Guess from CRATE: beyond +/-BAT_TREND_MIN_CENTIPCT %/h counts as moving, inside it is idle. The register is noisy
// near zero (one LSB is 0.208 %/h) and only an estimate over minutes, so this is a hint for the status icon.
#define BAT_TREND_MIN_CENTIPCT 100  // 1.00 %/h, about 5 LSB
bat_trend_t bat_trend_from_crate(uint16_t raw);

// ---- Optional ADC divider (mod 2: R_top from +BATT, R_bottom to GND, midpoint on GPIO19) ---------------------------
// adc_mv is the calibrated pin voltage (esp_adc calibration already applied by the driver). cell mV = adc_mv *
// (r_top + r_bottom) / r_bottom, rounded. The 100k+100k divider gives a ratio of 2. Returns -1 for a zero/negative
// resistor or a negative adc_mv; the result clamps to BAT_CELL_MAX_MV so a floating pin cannot report a huge cell.
#define BAT_CELL_MAX_MV 4600
int32_t bat_adc_to_cell_mv(int32_t adc_mv, uint32_t r_top_ohm, uint32_t r_bottom_ohm);
