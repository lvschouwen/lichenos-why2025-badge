#pragma once
// The sensors service's pure part (#69): unit conversion, plausibility checks and the `fw sensors` lines. No ESP-IDF,
// host-tested in host_tests/sensors_format_test.c.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The latest readings (sensors_get). The driver's own copy of the gate's lichen_sensors_t (#221, #245: a driver
// includes no gate header): the same fields in the same order, which lichen_os/lichen_sensors.c asserts field by field
// and copies over. Keep the two in step.
typedef struct {
    // BMI270, sampled every IMU period (default 100 ms). Axes as the chip reports them (not remapped to the screen).
    bool    imu_valid;   // the chip answered and its last reading is current
    float   accel_g[3];  // x, y, z in g (1 g = 9.81 m/s^2), range +-4 g
    float   gyro_dps[3]; // x, y, z in degrees per second, range +-1000 dps
    int64_t imu_us;      // esp_timer time of that reading
    // BME690, one forced measurement every environment period (default 1 s)
    bool    env_valid;
    // Measured inside the BME690. Its gas heater (300 C for 100 ms of every 1 s by default) and the badge's own warmth
    // reach it, so it reads somewhat above the room and humidity somewhat below; LICHEN_SENSORS_GAS_HEATER=n removes
    // the heater's part (and the gas reading)
    float   temperature_c;
    float   humidity_pct;
    float   pressure_hpa;
    bool    gas_valid; // the heater reached its temperature in the last measurement
    float   gas_kohm;  // gas resistance (higher = cleaner air; needs minutes of warm-up to settle)
    int64_t env_us;
    // The P4's internal temperature sensor (the chip, not the room: it runs warmer)
    bool    die_valid;
    float   die_c;
    int64_t die_us;
} sensors_snap_t;

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
int sensors_fmt_imu(char *buf, size_t cap, sensors_snap_t const *s, int64_t now_us, char const *why);
int sensors_fmt_env(char *buf, size_t cap, sensors_snap_t const *s, int64_t now_us, char const *why);
int sensors_fmt_die(char *buf, size_t cap, sensors_snap_t const *s, int64_t now_us, char const *why);
