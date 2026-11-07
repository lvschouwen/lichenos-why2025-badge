#pragma once
// The sensors service's OS side (#69). Apps use lichen_sensors.h, which lichen_os implements over the calls below
// (#221: a driver never defines a gate function); main starts the service (lichen_sensors_start) after the keyboard
// has the bus, and registers `fw sensors`.

#include "sensors_format.h" // sensors_snap_t

#include <stdbool.h>
#include <stdint.h>

// lichen_sensors.h's calls, with the same rules (see there); sensors_snap_t is lichen_sensors_t field for field.
// sensors_get copies with memcpy, so lichen_os may pass the gate's struct (asserted identical) straight in.
void sensors_get(sensors_snap_t *out);
void sensors_start(void);
void sensors_stop(void);
bool sensors_running(void);
void sensors_set_periods(uint32_t imu_ms, uint32_t env_ms);

void sensors_register_console(void);

// badge_fw u8, 0 = boot leaves the service stopped (`fw sensors boot off`, a #115 diagnosis); missing = start
#define SENSORS_BOOT_KEY "sens_boot"
bool sensors_boot_start(void);
// Where that flag is kept (the settings service, #224): sensors open no settings of their own. Until bound the default
// (start) applies and `fw sensors boot` reports not saved. Call before sensors_boot_start().
void sensors_bind_store(uint8_t (*get)(char const *key, uint8_t def), bool (*set)(char const *key, uint8_t value),
                        bool (*erase)(char const *key));

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
