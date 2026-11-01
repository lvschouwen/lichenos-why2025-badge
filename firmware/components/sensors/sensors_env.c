// BME690 through Bosch's BME69x_SensorAPI (third_party/BME69x_SensorAPI), forced mode: one measurement per trigger,
// the chip sleeps in between
#include "bme69x.h"
#include "sdkconfig.h"
#include "sensors_chips.h"
#include "sensors_format.h"

static struct bme69x_dev        bme;
static struct bme69x_conf       conf;
static struct bme69x_heatr_conf heater;
static i2c_master_dev_handle_t  dev;

static BME69X_INTF_RET_TYPE bme_read(uint8_t reg, uint8_t *data, uint32_t len, void *intf) {
    return sensors_bus_read(*(i2c_master_dev_handle_t *)intf, reg, data, len);
}

static BME69X_INTF_RET_TYPE bme_write(uint8_t reg, uint8_t const *data, uint32_t len, void *intf) {
    return sensors_bus_write(*(i2c_master_dev_handle_t *)intf, reg, data, len);
}

static void bme_delay(uint32_t us, void *intf) {
    (void)intf;
    sensors_delay_us(us);
}

bool sensors_env_init(i2c_master_bus_handle_t bus, int *err) {
    if (!sensors_bus_dev(bus, SENSORS_ENV_ADDR, &dev)) {
        *err = BME69X_E_DEV_NOT_FOUND;
        return false;
    }
    bme = (struct bme69x_dev){
        .intf     = BME69X_I2C_INTF,
        .intf_ptr = &dev,
        .read     = bme_read,
        .write    = bme_write,
        .delay_us = bme_delay,
        .amb_temp = 25, // the heater resistance for 300 C is computed once from this ambient temperature
    };
    int8_t r = bme69x_init(&bme); // checks the chip id (0x61, BME69x family)
    if (r == BME69X_OK) {
        conf = (struct bme69x_conf){
            .os_hum  = BME69X_OS_1X,
            .os_temp = BME69X_OS_2X,
            .os_pres = BME69X_OS_4X,
            .filter  = BME69X_FILTER_OFF,
            .odr     = BME69X_ODR_NONE,
        };
        r = bme69x_set_conf(&conf, &bme);
    }
    if (r == BME69X_OK) {
#if CONFIG_LICHEN_SENSORS_GAS_HEATER
        heater = (struct bme69x_heatr_conf){.enable     = BME69X_ENABLE,
                                            .heatr_temp = 300,
                                            .heatr_dur  = CONFIG_LICHEN_SENSORS_HEATER_MS};
#else
        heater = (struct bme69x_heatr_conf){.enable = BME69X_DISABLE}; // no gas reading, no self-heating
#endif
        r = bme69x_set_heatr_conf(BME69X_FORCED_MODE, &heater, &bme);
    }
    *err = r;
    return r == BME69X_OK;
}

uint32_t sensors_env_trigger(void) {
    if (bme69x_set_op_mode(BME69X_FORCED_MODE, &bme) != BME69X_OK) {
        return 0;
    }
    uint32_t heat_us = heater.enable ? (uint32_t)heater.heatr_dur * 1000u : 0;
    return bme69x_get_meas_dur(BME69X_FORCED_MODE, &conf, &bme) + heat_us;
}

sensors_read_t sensors_env_read(float *temp_c, float *rh_pct, float *hpa, float *kohm, bool *gas_ok) {
    struct bme69x_data d = {0};
    uint8_t            n = 0;
    int8_t             r = bme69x_get_data(BME69X_FORCED_MODE, &d, &n, &bme);
    if (r == BME69X_W_NO_NEW_DATA || (r == BME69X_OK && n == 0)) {
        return SENSORS_READ_NONE;
    }
    if (r != BME69X_OK) {
        return SENSORS_READ_ERR;
    }
    *temp_c = d.temperature;
    *rh_pct = d.humidity;
    *hpa    = sensors_pa_to_hpa(d.pressure);
    *kohm   = sensors_ohm_to_kohm(d.gas_resistance);
    *gas_ok = (d.status & BME69X_GASM_VALID_MSK) && (d.status & BME69X_HEAT_STAB_MSK);
    return SENSORS_READ_OK;
}

i2c_master_dev_handle_t sensors_env_dev(void) {
    return dev;
}
