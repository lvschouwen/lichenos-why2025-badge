// lichen_sensors.h for apps and the shell (#221): the gate's calls, defined here over the sensors driver (L2) so no
// caller links to a driver; the driver's own console keeps its sensors_* names
#include "lichen_sensors.h"

#include "sensors.h"

void lichen_sensors_get(lichen_sensors_t *out) {
    sensors_get(out);
}

void lichen_sensors_start(void) {
    sensors_start();
}

void lichen_sensors_stop(void) {
    sensors_stop();
}

bool lichen_sensors_running(void) {
    return sensors_running();
}

void lichen_sensors_set_periods(uint32_t imu_ms, uint32_t env_ms) {
    sensors_set_periods(imu_ms, env_ms);
}
