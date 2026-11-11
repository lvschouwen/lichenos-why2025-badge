#pragma once
// What the sensor chips are configured to, as the chips report it (#318): the pure part, host-tested in
// host_tests/reg_sensors_test.c. No ESP-IDF: sensors_imu.c and sensors_env.c read the registers, this turns them into
// the values of the registry's sens.* keys (reg_p4_sensors.c).

#include <stdbool.h>
#include <stdint.h>

// The key order is the low byte of the REG_ID_SENS_* ids (the test pins it)
typedef enum {
    SENSORS_CFG_ACC_ODR,
    SENSORS_CFG_ACC_RANGE,
    SENSORS_CFG_ACC_BWP,
    SENSORS_CFG_ACC_PERF,
    SENSORS_CFG_GYR_ODR,
    SENSORS_CFG_GYR_RANGE,
    SENSORS_CFG_GYR_BWP,
    SENSORS_CFG_GYR_PERF,
    SENSORS_CFG_ENV_OS_T,
    SENSORS_CFG_ENV_OS_P,
    SENSORS_CFG_ENV_OS_H,
    SENSORS_CFG_ENV_FILTER,
    SENSORS_CFG_HEAT_ON,
    SENSORS_CFG_HEAT_TEMP,
    SENSORS_CFG_HEAT_MS,
    SENSORS_CFG_KEYS,
} sensors_cfg_key_t;

// One read-back of both chips; a chip's fields count only while its _ok is set (never read, or the read failed)
typedef struct {
    bool     imu_ok, env_ok;
    uint8_t  acc_odr, acc_range, acc_bwp, acc_perf; // the chip's register codes
    uint8_t  gyr_odr, gyr_range, gyr_bwp, gyr_perf;
    uint8_t  env_os_t, env_os_p, env_os_h, env_filter;
    bool     heat_on;
    uint16_t heat_c;  // decoded from the heater resistance register
    uint16_t heat_ms; // decoded from the gas wait register
} sensors_cfg_t;

// The key's value; false for an unknown key or when the chip it belongs to is not ok (a clear failure, never zeros)
bool sensors_cfg_value(sensors_cfg_t const *c, unsigned key, uint32_t *out);

// BME690 register decoding. gas_wait_0: 6-bit value times 1, 4, 16 or 64 ms (the top two bits); the heater is on when
// ctrl_gas_0 bit 3 (heat off) is clear and ctrl_gas_1 run_gas (bits 5:4) is set.
uint32_t sensors_heat_ms(uint8_t gas_wait);
bool     sensors_heat_on(uint8_t ctrl_gas_0, uint8_t ctrl_gas_1);

// The target temperature (C) behind res_heat_0: Bosch's float conversion (calc_res_heat in bme69x.c) solved for the
// temperature, with the chip's calibration (par_g1..3, res_heat_range, res_heat_val) and the ambient temperature
// the driver was initialised with. The register is 8 bit, so the result is within a few C of what was set.
typedef struct {
    int8_t  par_g1, par_g3, res_heat_val;
    int16_t par_g2;
    uint8_t res_heat_range;
} sensors_heat_cal_t;
uint32_t sensors_heat_temp_c(uint8_t res_heat, sensors_heat_cal_t const *cal, int amb_c);
