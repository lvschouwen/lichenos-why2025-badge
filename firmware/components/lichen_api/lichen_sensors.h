#pragma once
// The badge's sensors (#69): the BMI270 IMU (0x69) and BME690 environment sensor (0x76) on the shared I2C bus, and
// the P4's own die temperature. The OS owns the chips and samples them from one task (components/sensors); apps read
// the latest snapshot here and never talk to the chips or the bus (#84). Implemented by the sensors component, which
// the firmware always links. Plain C types only, so pure code and host tests can use the struct.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
} lichen_sensors_t;

// Copies the latest readings under a short spinlock: call it from a task, never from an ISR. Never blocks on the bus;
// all valid flags are false before the first sample, while the service is stopped, and for a chip that is missing or
// stopped answering.
void lichen_sensors_get(lichen_sensors_t *out);

// The OS starts the service at boot. Stop puts both chips to sleep and clears the valid flags; start wakes them
// again. Both return at once (the sampling task does the bus work).
void lichen_sensors_start(void);
void lichen_sensors_stop(void);
bool lichen_sensors_running(void);

// Sampling periods in ms, clamped to 20-1000 (IMU) and 1000-60000 (environment); 0 keeps the current one. The
// defaults come from Kconfig (LICHEN_SENSORS_IMU_PERIOD_MS, LICHEN_SENSORS_ENV_PERIOD_MS).
void lichen_sensors_set_periods(uint32_t imu_ms, uint32_t env_ms);

#ifdef __cplusplus
}
#endif
