#pragma once
// The sensors service's OS side (#69). Apps use lichen_sensors.h; main starts the service (lichen_sensors_start)
// after the keyboard has the bus, and registers `fw sensors`.

#include <stdbool.h>
#include <stdint.h>

void sensors_register_console(void);

// What `fw sensors raw` prints besides the registers
typedef struct {
    bool     imu_up, env_up, die_up;   // initialised and answering
    int      imu_err, env_err;         // Bosch result code of the last init (0 = ok, -3 = not found)
    uint32_t imu_samples, env_samples; // good readings
    uint32_t imu_fails, env_fails;     // failed or implausible readings
    uint32_t imu_inits, env_inits;     // init attempts (the first at start, then after a chip stopped answering)
    int16_t  imu_raw[6];               // last accel x/y/z, gyro x/y/z counts
    uint32_t imu_ms, env_ms;           // sampling periods
    uint32_t stack_free;               // bytes the task never used of its stack
} sensors_stats_t;

void sensors_stats(sensors_stats_t *out);
