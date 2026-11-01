// BMI270 through Bosch's BMI270_SensorAPI (third_party/BMI270_SensorAPI)
#include "bmi270.h"
#include "sensors_chips.h"
#include "sensors_format.h"

static struct bmi2_dev         bmi;
static i2c_master_dev_handle_t dev;

static BMI2_INTF_RETURN_TYPE bmi_read(uint8_t reg, uint8_t *data, uint32_t len, void *intf) {
    return sensors_bus_read(*(i2c_master_dev_handle_t *)intf, reg, data, len);
}

static BMI2_INTF_RETURN_TYPE bmi_write(uint8_t reg, uint8_t const *data, uint32_t len, void *intf) {
    return sensors_bus_write(*(i2c_master_dev_handle_t *)intf, reg, data, len);
}

static void bmi_delay(uint32_t us, void *intf) {
    (void)intf;
    sensors_delay_us(us);
}

static int8_t configure(void) {
    struct bmi2_sens_config cfg[2] = {{.type = BMI2_ACCEL}, {.type = BMI2_GYRO}};
    int8_t                  r      = bmi2_get_sensor_config(cfg, 2, &bmi);
    if (r != BMI2_OK) {
        return r;
    }
    cfg[0].cfg.acc.odr         = BMI2_ACC_ODR_100HZ;
    cfg[0].cfg.acc.range       = BMI2_ACC_RANGE_4G;
    cfg[0].cfg.acc.bwp         = BMI2_ACC_NORMAL_AVG4;
    cfg[0].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
    cfg[1].cfg.gyr.odr         = BMI2_GYR_ODR_100HZ;
    cfg[1].cfg.gyr.range       = BMI2_GYR_RANGE_1000;
    cfg[1].cfg.gyr.bwp         = BMI2_GYR_NORMAL_MODE;
    cfg[1].cfg.gyr.noise_perf  = BMI2_POWER_OPT_MODE;
    cfg[1].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;
    r                          = bmi2_set_sensor_config(cfg, 2, &bmi);
    if (r == BMI2_OK) {
        r = sensors_imu_power(true) ? BMI2_OK : BMI2_E_COM_FAIL;
    }
    // Advanced power save adds 450 us waits between register accesses; off, so a read is one short transfer
    return r == BMI2_OK ? bmi2_set_adv_power_save(BMI2_DISABLE, &bmi) : r;
}

bool sensors_imu_init(i2c_master_bus_handle_t bus, int *err) {
    if (!sensors_bus_dev(bus, SENSORS_IMU_ADDR, &dev)) {
        *err = BMI2_E_DEV_NOT_FOUND;
        return false;
    }
    bmi = (struct bmi2_dev){
        .intf           = BMI2_I2C_INTF,
        .intf_ptr       = &dev,
        .read           = bmi_read,
        .write          = bmi_write,
        .delay_us       = bmi_delay,
        .read_write_len = 32, // config upload in 32-byte writes: the keyboard gets the bus between them
    };
    int8_t r = bmi270_init(&bmi);
    if (r == BMI2_OK) {
        r = configure();
    }
    *err = r;
    return r == BMI2_OK;
}

bool sensors_imu_power(bool on) {
    uint8_t const list[2] = {BMI2_ACCEL, BMI2_GYRO};
    return (on ? bmi2_sensor_enable(list, 2, &bmi) : bmi2_sensor_disable(list, 2, &bmi)) == BMI2_OK;
}

sensors_read_t sensors_imu_read(float accel_g[3], float gyro_dps[3], int16_t raw[6]) {
    struct bmi2_sens_data d = {0};
    if (bmi2_get_sensor_data(&d, &bmi) != BMI2_OK) {
        return SENSORS_READ_ERR;
    }
    if ((d.status & (BMI2_DRDY_ACC | BMI2_DRDY_GYR)) != (BMI2_DRDY_ACC | BMI2_DRDY_GYR)) {
        return SENSORS_READ_NONE; // read faster than the 100 Hz output rate
    }
    int16_t const v[6] = {d.acc.x, d.acc.y, d.acc.z, d.gyr.x, d.gyr.y, d.gyr.z};
    for (int i = 0; i < 6; i++) {
        raw[i] = v[i];
    }
    for (int i = 0; i < 3; i++) {
        accel_g[i]  = sensors_accel_g(v[i], SENSORS_IMU_RANGE_G);
        gyro_dps[i] = sensors_gyro_dps(v[3 + i], SENSORS_IMU_RANGE_DPS);
    }
    return SENSORS_READ_OK;
}

i2c_master_dev_handle_t sensors_imu_dev(void) {
    return dev;
}
