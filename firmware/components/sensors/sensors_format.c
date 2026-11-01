#include "sensors_format.h"

#include <stdio.h>

float sensors_accel_g(int16_t raw, float range_g) {
    return (float)raw * range_g / 32768.0f;
}

float sensors_gyro_dps(int16_t raw, float range_dps) {
    return (float)raw * range_dps / 32768.0f;
}

float sensors_pa_to_hpa(float pa) {
    return pa / 100.0f;
}

float sensors_ohm_to_kohm(float ohm) {
    return ohm / 1000.0f;
}

uint32_t sensors_period(uint32_t req, uint32_t cur, uint32_t lo, uint32_t hi) {
    if (req == 0) {
        return cur;
    }
    return req < lo ? lo : req > hi ? hi : req;
}

static bool within(float v, float lo, float hi) {
    return v >= lo && v <= hi; // false for NaN too
}

bool sensors_env_plausible(float temp_c, float rh_pct, float hpa) {
    return within(temp_c, -40.0f, 85.0f) && within(rh_pct, 0.0f, 100.0f) && within(hpa, 300.0f, 1100.0f);
}

bool sensors_die_plausible(float c) {
    return within(c, -40.0f, 125.0f);
}

bool sensors_imu_plausible(float const accel_g[3], float range_g, float const gyro_dps[3], float range_dps) {
    for (int i = 0; i < 3; i++) {
        if (!within(accel_g[i], -range_g, range_g) || !within(gyro_dps[i], -range_dps, range_dps)) {
            return false;
        }
    }
    return true;
}

// snprintf's return value is what it wanted to write; callers get what is actually in buf
static int fitted(int n, size_t cap) {
    if (n < 0 || cap == 0) {
        return 0;
    }
    return (size_t)n >= cap ? (int)cap - 1 : n;
}

static double age_s(int64_t now_us, int64_t at_us) {
    return now_us > at_us ? (double)(now_us - at_us) / 1e6 : 0.0;
}

int sensors_fmt_imu(char *buf, size_t cap, lichen_sensors_t const *s, int64_t now_us, char const *why) {
    if (!s->imu_valid) {
        return fitted(snprintf(buf, cap, "imu: %s", why ? why : "no data"), cap);
    }
    return fitted(
        snprintf(
            buf,
            cap,
            "imu: accel %+.2f %+.2f %+.2f g, gyro %+.1f %+.1f %+.1f dps (%.1f s ago)",
            (double)s->accel_g[0],
            (double)s->accel_g[1],
            (double)s->accel_g[2],
            (double)s->gyro_dps[0],
            (double)s->gyro_dps[1],
            (double)s->gyro_dps[2],
            age_s(now_us, s->imu_us)
        ),
        cap
    );
}

int sensors_fmt_env(char *buf, size_t cap, lichen_sensors_t const *s, int64_t now_us, char const *why) {
    if (!s->env_valid) {
        return fitted(snprintf(buf, cap, "env: %s", why ? why : "no data"), cap);
    }
    char gas[24] = "gas --";
    if (s->gas_valid) {
        snprintf(gas, sizeof gas, "gas %.1f kOhm", (double)s->gas_kohm);
    }
    return fitted(
        snprintf(
            buf,
            cap,
            "env: %.2f C, %.1f %%RH, %.2f hPa, %s (%.1f s ago)",
            (double)s->temperature_c,
            (double)s->humidity_pct,
            (double)s->pressure_hpa,
            gas,
            age_s(now_us, s->env_us)
        ),
        cap
    );
}

int sensors_fmt_die(char *buf, size_t cap, lichen_sensors_t const *s, int64_t now_us, char const *why) {
    if (!s->die_valid) {
        return fitted(snprintf(buf, cap, "die: %s", why ? why : "no data"), cap);
    }
    return fitted(snprintf(buf, cap, "die: %.1f C (P4) (%.1f s ago)", (double)s->die_c, age_s(now_us, s->die_us)), cap);
}
