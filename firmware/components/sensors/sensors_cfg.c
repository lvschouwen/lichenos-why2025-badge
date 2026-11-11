#include "sensors_cfg.h"

uint32_t sensors_heat_ms(uint8_t gas_wait) {
    return (uint32_t)(gas_wait & 0x3F) << (2 * (gas_wait >> 6));
}

bool sensors_heat_on(uint8_t ctrl_gas_0, uint8_t ctrl_gas_1) {
    return !(ctrl_gas_0 & 0x08) && ((ctrl_gas_1 >> 4) & 0x03) != 0;
}

uint32_t sensors_heat_temp_c(uint8_t res_heat, sensors_heat_cal_t const *cal, int amb_c) {
    float var1 = ((float)cal->par_g1 / 16.0f) + 49.0f;
    float var2 = (((float)cal->par_g2 / 32768.0f) * 0.0005f) + 0.00235f;
    float var3 = (float)cal->par_g3 / 1024.0f;
    // calc_res_heat: res = 3.4 * (var5 * 4 / (4 + range) / (1 + val * 0.002) - 25), var5 = var1 (1 + var2 T) + var3 amb
    float var5 = ((float)res_heat / 3.4f + 25.0f) * (4.0f + (float)cal->res_heat_range) / 4.0f *
                 (1.0f + (float)cal->res_heat_val * 0.002f);
    float t = ((var5 - var3 * (float)amb_c) / var1 - 1.0f) / var2;
    return t < 0.0f ? 0 : t > 400.0f ? 400 : (uint32_t)(t + 0.5f);
}

static bool imu_value(sensors_cfg_t const *c, unsigned key, uint32_t *out) {
    uint8_t const v[] = {
        [SENSORS_CFG_ACC_ODR]   = c->acc_odr,
        [SENSORS_CFG_ACC_RANGE] = c->acc_range,
        [SENSORS_CFG_ACC_BWP]   = c->acc_bwp,
        [SENSORS_CFG_ACC_PERF]  = c->acc_perf,
        [SENSORS_CFG_GYR_ODR]   = c->gyr_odr,
        [SENSORS_CFG_GYR_RANGE] = c->gyr_range,
        [SENSORS_CFG_GYR_BWP]   = c->gyr_bwp,
        [SENSORS_CFG_GYR_PERF]  = c->gyr_perf,
    };
    *out = v[key];
    return c->imu_ok;
}

static bool env_value(sensors_cfg_t const *c, unsigned key, uint32_t *out) {
    switch (key) {
    case SENSORS_CFG_ENV_OS_T:
        *out = c->env_os_t;
        break;
    case SENSORS_CFG_ENV_OS_P:
        *out = c->env_os_p;
        break;
    case SENSORS_CFG_ENV_OS_H:
        *out = c->env_os_h;
        break;
    case SENSORS_CFG_ENV_FILTER:
        *out = c->env_filter;
        break;
    case SENSORS_CFG_HEAT_ON:
        *out = c->heat_on;
        break;
    case SENSORS_CFG_HEAT_TEMP:
        *out = c->heat_c;
        break;
    default:
        *out = c->heat_ms;
        break;
    }
    return c->env_ok;
}

bool sensors_cfg_value(sensors_cfg_t const *c, unsigned key, uint32_t *out) {
    if (key >= SENSORS_CFG_KEYS) {
        return false;
    }
    return key <= SENSORS_CFG_GYR_PERF ? imu_value(c, key, out) : env_value(c, key, out);
}
