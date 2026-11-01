// Host test: the sensors service's conversions, plausibility checks and `fw sensors` lines (#69)
#include "sensors_format.h"

#include <stdio.h>

#include <math.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)

int main(void) {
    // Full scale: 32767 counts is just under +range, -32768 exactly -range; 8192 counts at +-4 g = 1 g
    CHECK(NEAR(sensors_accel_g(8192, 4.0f), 1.0f));
    CHECK(NEAR(sensors_accel_g(-32768, 4.0f), -4.0f));
    CHECK(sensors_accel_g(32767, 4.0f) < 4.0f && sensors_accel_g(32767, 4.0f) > 3.999f);
    CHECK(NEAR(sensors_gyro_dps(16384, 1000.0f), 500.0f));
    CHECK(NEAR(sensors_gyro_dps(0, 1000.0f), 0.0f));
    CHECK(NEAR(sensors_pa_to_hpa(101325.0f), 1013.25f));
    CHECK(NEAR(sensors_ohm_to_kohm(120500.0f), 120.5f));

    // Periods: 0 keeps the current one, the rest is clamped
    CHECK(sensors_period(0, 100, SENSORS_IMU_MIN_MS, SENSORS_IMU_MAX_MS) == 100);
    CHECK(sensors_period(5, 100, SENSORS_IMU_MIN_MS, SENSORS_IMU_MAX_MS) == 20);
    CHECK(sensors_period(50, 100, SENSORS_IMU_MIN_MS, SENSORS_IMU_MAX_MS) == 50);
    CHECK(sensors_period(99999, 1000, SENSORS_ENV_MIN_MS, SENSORS_ENV_MAX_MS) == 60000);

    // Plausibility
    CHECK(sensors_env_plausible(21.5f, 45.0f, 1013.0f));
    CHECK(!sensors_env_plausible(-41.0f, 45.0f, 1013.0f));
    CHECK(!sensors_env_plausible(21.5f, 100.5f, 1013.0f));
    CHECK(!sensors_env_plausible(21.5f, 45.0f, 0.0f)); // an all-zero read
    CHECK(!sensors_env_plausible(NAN, 45.0f, 1013.0f));
    CHECK(sensors_die_plausible(45.0f) && !sensors_die_plausible(200.0f) && !sensors_die_plausible(NAN));
    float acc[3] = {0.0f, 0.1f, 1.0f}, gyr[3] = {0.5f, -0.5f, 0.0f};
    CHECK(sensors_imu_plausible(acc, 4.0f, gyr, 1000.0f));
    acc[2] = 4.5f;
    CHECK(!sensors_imu_plausible(acc, 4.0f, gyr, 1000.0f));

    // Lines
    lichen_sensors_t s = {0};
    char             line[160];
    CHECK(sensors_fmt_imu(line, sizeof line, &s, 0, "not found") == 14 && strcmp(line, "imu: not found") == 0);
    sensors_fmt_env(line, sizeof line, &s, 0, NULL);
    CHECK(strcmp(line, "env: no data") == 0);
    sensors_fmt_die(line, sizeof line, &s, 0, "stopped");
    CHECK(strcmp(line, "die: stopped") == 0);

    s.imu_valid  = true;
    s.accel_g[0] = 0.012f, s.accel_g[1] = -0.02f, s.accel_g[2] = 1.004f;
    s.gyro_dps[0] = 0.12f, s.gyro_dps[1] = -0.26f, s.gyro_dps[2] = 0.0f;
    s.imu_us = 1000000;
    sensors_fmt_imu(line, sizeof line, &s, 1050000, NULL);
    CHECK(strcmp(line, "imu: accel +0.01 -0.02 +1.00 g, gyro +0.1 -0.3 +0.0 dps (0.1 s ago)") == 0);

    s.env_valid = true, s.temperature_c = 22.456f, s.humidity_pct = 45.06f, s.pressure_hpa = 1013.254f;
    s.env_us = 2000000;
    sensors_fmt_env(line, sizeof line, &s, 2400000, NULL);
    CHECK(strcmp(line, "env: 22.46 C, 45.1 %RH, 1013.25 hPa, gas -- (0.4 s ago)") == 0);
    s.gas_valid = true, s.gas_kohm = 120.54f;
    sensors_fmt_env(line, sizeof line, &s, 2400000, NULL);
    CHECK(strcmp(line, "env: 22.46 C, 45.1 %RH, 1013.25 hPa, gas 120.5 kOhm (0.4 s ago)") == 0);

    s.die_valid = true, s.die_c = 41.23f, s.die_us = 5000000;
    sensors_fmt_die(line, sizeof line, &s, 4000000, NULL); // a clock read before the sample: age 0, not negative
    CHECK(strcmp(line, "die: 41.2 C (P4) (0.0 s ago)") == 0);

    // A short buffer is cut, NUL-terminated, and the length is what it holds
    char small[10];
    CHECK(sensors_fmt_die(small, sizeof small, &s, 5000000, NULL) == 9 && strcmp(small, "die: 41.2") == 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("sensors_format_test ok\n");
    return 0;
}
