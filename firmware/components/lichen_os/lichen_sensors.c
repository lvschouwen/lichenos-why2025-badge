// lichen_sensors.h for apps and the shell (#221): the gate's calls, defined here over the sensors driver (L2) so no
// caller links to a driver; the driver's own console keeps its sensors_* names. The driver keeps its own snapshot type
// (sensors_snap_t, #245: it includes no gate header); the asserts below keep it equal to the gate's, so the caller's
// struct is filled in place (sensors_get copies bytes with memcpy, never through a sensors_snap_t lvalue).
#include "lichen_sensors.h"
#include "lichen_sys_app.h"

#include "sensors.h"

#include <stddef.h>
#include <string.h>

// A mirror field matches when it sits at the same offset with the same type (so a float never mirrors an int32)
#define MIRROR_FIELD(field)                                                                                            \
    _Static_assert(offsetof(lichen_sensors_t, field) == offsetof(sensors_snap_t, field) &&                             \
                       __builtin_types_compatible_p(__typeof__(((lichen_sensors_t *)0)->field),                       \
                                                    __typeof__(((sensors_snap_t *)0)->field)),                         \
                   "the gate's lichen_sensors_t." #field " follows sensors_snap_t")
_Static_assert(sizeof(lichen_sensors_t) == sizeof(sensors_snap_t), "the gate's sensor readings follow the driver's");
MIRROR_FIELD(imu_valid);
MIRROR_FIELD(accel_g);
MIRROR_FIELD(gyro_dps);
MIRROR_FIELD(imu_us);
MIRROR_FIELD(env_valid);
MIRROR_FIELD(temperature_c);
MIRROR_FIELD(humidity_pct);
MIRROR_FIELD(pressure_hpa);
MIRROR_FIELD(gas_valid);
MIRROR_FIELD(gas_kohm);
MIRROR_FIELD(env_us);
MIRROR_FIELD(die_valid);
MIRROR_FIELD(die_c);
MIRROR_FIELD(die_us);

// #229: an app that did not declare the sensors reads nothing valid and starts or stops nothing; the OS's own callers
// (the thermal watchdog, the notices, main) run as the OS and always pass
void lichen_sensors_get(lichen_sensors_t *out) {
    if (!lichen_sys_caps_self_ok(CAP_SENSORS, "lichen_sensors_get")) {
        memset(out, 0, sizeof(*out));
        return;
    }
    sensors_get((sensors_snap_t *)(void *)out);
}

void lichen_sensors_start(void) {
    if (lichen_sys_caps_self_ok(CAP_SENSORS, "lichen_sensors_start")) {
        sensors_start();
    }
}

void lichen_sensors_stop(void) {
    if (lichen_sys_caps_self_ok(CAP_SENSORS, "lichen_sensors_stop")) {
        sensors_stop();
    }
}

bool lichen_sensors_running(void) {
    return sensors_running();
}

void lichen_sensors_set_periods(uint32_t imu_ms, uint32_t env_ms) {
    if (!lichen_sys_caps_self_ok(CAP_SENSORS, "lichen_sensors_set_periods")) {
        return;
    }
    sensors_set_periods(imu_ms, env_ms);
}
