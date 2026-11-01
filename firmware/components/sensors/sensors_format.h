#pragma once
// The sensors service's pure part (#69): unit conversion, plausibility checks and the `fw sensors` lines. No ESP-IDF,
// host-tested in host_tests/sensors_format_test.c.

#include "lichen_sensors.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SENSORS_IMU_MIN_MS 20
#define SENSORS_IMU_MAX_MS 1000
#define SENSORS_ENV_MIN_MS 1000
#define SENSORS_ENV_MAX_MS 60000

// A 16-bit two's-complement count at full scale +-range (g or dps)
float sensors_accel_g(int16_t raw, float range_g);
float sensors_gyro_dps(int16_t raw, float range_dps);
float sensors_pa_to_hpa(float pa);
float sensors_ohm_to_kohm(float ohm);

// A requested period: 0 keeps cur, anything else is clamped to lo..hi
uint32_t sensors_period(uint32_t req, uint32_t cur, uint32_t lo, uint32_t hi);

// Readings outside what the chips can measure mean a bad transfer or a chip in a strange state: not shown as valid.
// BME690: -40..85 C, 0..100 %RH, 300..1100 hPa. P4 die: -40..125 C. Accel within +-range, gyro within +-range.
bool sensors_env_plausible(float temp_c, float rh_pct, float hpa);
bool sensors_die_plausible(float c);
bool sensors_imu_plausible(float const accel_g[3], float range_g, float const gyro_dps[3], float range_dps);

// One `fw sensors` line each, always NUL-terminated; returns the length written (cut to cap - 1). An invalid reading
// prints why (e.g. "not found", "stopped"); now_us gives the age of a valid one.
int sensors_fmt_imu(char *buf, size_t cap, lichen_sensors_t const *s, int64_t now_us, char const *why);
int sensors_fmt_env(char *buf, size_t cap, lichen_sensors_t const *s, int64_t now_us, char const *why);
int sensors_fmt_die(char *buf, size_t cap, lichen_sensors_t const *s, int64_t now_us, char const *why);
