#pragma once
// The sensors service's chip glue (#69): Bosch's BMI270 and BME69x SensorAPIs (third_party/, BSD-3-Clause) on the
// shared badge bus. Only the sampling task calls these, so the Bosch device structs need no lock. Every bus access
// is its own short I2C transaction with a timeout (the keyboard chip shares the bus): the driver's bus lock lets the
// keyboard in between, and waits (heater, soft reset) happen with the bus free.

#include "driver/i2c_master.h"

#include <stdbool.h>
#include <stdint.h>

#define SENSORS_IMU_ADDR      0x69 // BMI270, SDO high
#define SENSORS_ENV_ADDR      0x76 // BME690, SDO low
#define SENSORS_IMU_RANGE_G   4.0f
#define SENSORS_IMU_RANGE_DPS 1000.0f

// A device handle for addr when it answers a probe (once; later calls return the same handle), else NULL
i2c_master_dev_handle_t sensors_bus_dev(i2c_master_bus_handle_t bus, uint8_t addr, i2c_master_dev_handle_t *slot);
// One register read or write; 0 when the transfer went through
int8_t                  sensors_bus_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, uint32_t len);
int8_t                  sensors_bus_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t const *data, uint32_t len);
void                    sensors_delay_us(uint32_t us);

typedef enum { SENSORS_READ_OK, SENSORS_READ_NONE, SENSORS_READ_ERR } sensors_read_t;

// BMI270: soft reset, config upload (8 KB in 32-byte writes), accel 100 Hz +-4 g, gyro 100 Hz +-1000 dps. The Bosch
// result code of the last failure is in *err.
bool                    sensors_imu_init(i2c_master_bus_handle_t bus, int *err);
sensors_read_t          sensors_imu_read(float accel_g[3], float gyro_dps[3], int16_t raw[6]);
bool                    sensors_imu_power(bool on); // accel and gyro on, or both off (suspend)
i2c_master_dev_handle_t sensors_imu_dev(void);

// BME690: soft reset, calibration, forced mode with the heater at 300 C for 100 ms
bool                    sensors_env_init(i2c_master_bus_handle_t bus, int *err);
// Starts one forced measurement; returns how long it takes in us, 0 on a bus error
uint32_t                sensors_env_trigger(void);
sensors_read_t          sensors_env_read(float *temp_c, float *rh_pct, float *hpa, float *kohm, bool *gas_ok);
i2c_master_dev_handle_t sensors_env_dev(void);
