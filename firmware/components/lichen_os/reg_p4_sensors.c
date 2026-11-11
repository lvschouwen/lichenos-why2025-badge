#include "reg_p4.h"
#include "ui_lock_owner.h"

// The P4's registry keys of domain 0x13: sens.* (#318, row file split #315): the BMI270 and BME690 configuration read
// back from the chips by the sensors task (sensors_cfg_get). All RO. An ENUM's value is the chip's own register code,
// so the name tables are indexed by it; a code the table lacks (a reserved one) is a failed read, not a wrong name.
// Keep ESP calls out of this file (the getter lives in the sensors component, declared extern here, stubbed in
// host_tests/reg_stubs_sensors.c) so the host test can link it.
extern bool sensors_cfg_get(unsigned key, bool live, uint32_t *out);
// The console may wait for a live read-back, the LVGL task never does (as lichen_reg.c's may_wait)
extern ui_lock_verdict_t ui_service_lock_check(void);

static char const *const odr_acc[]     = {"reserved", "0.78Hz", "1.56Hz", "3.12Hz", "6.25Hz", "12.5Hz", "25Hz",
                                          "50Hz",     "100Hz",  "200Hz",  "400Hz",  "800Hz",  "1600Hz"};
static char const *const odr_gyr[]     = {"reserved", "reserved", "reserved", "reserved", "reserved", "reserved", "25Hz",
                                          "50Hz",     "100Hz",    "200Hz",    "400Hz",    "800Hz",    "1600Hz",
                                          "3200Hz"};
static char const *const range_acc[]   = {"2g", "4g", "8g", "16g"};
static char const *const range_gyr[]   = {"2000dps", "1000dps", "500dps", "250dps", "125dps"};
static char const *const bwp_acc[]     = {"osr4_avg1", "osr2_avg2", "normal_avg4", "cic_avg8",
                                          "res_avg16", "res_avg32", "res_avg64",   "res_avg128"};
static char const *const bwp_gyr[]     = {"osr4", "osr2", "normal", "reserved"};
static char const *const perf[]        = {"power", "perf"};
static char const *const oversample[]  = {"off", "1x", "2x", "4x", "8x", "16x"};
static char const *const filter_size[] = {"off", "1", "3", "7", "15", "31", "63", "127"};

static reg_status_t sens_get(reg_entry_t const *e, reg_value_t *out) {
    uint32_t v = 0;
    // The domain's low byte is the sensors_cfg_key_t; live only off the LVGL task
    if (!sensors_cfg_get(e->id & 0xFFu, ui_service_lock_check() != UI_LOCK_OK, &v)) {
        return REG_FAILED;
    }
    if (e->type == REG_T_ENUM && v > (uint32_t)e->max) {
        return REG_FAILED; // a code beyond the table: reg_get only checks the type's width
    }
    out->u = v;
    return REG_OK;
}

#define ENUM_ROW(id_, name_, names_)                                                                                   \
    {.id = (id_), .type = REG_T_ENUM, .flags = REG_F_RO, .name = (name_), .max = sizeof(names_) / sizeof((names_)[0]) - 1, \
     .enum_names = (names_), .get_applied = sens_get}

static reg_entry_t const rows[] = {
    ENUM_ROW(REG_ID_SENS_ACC_ODR, "sens.acc_odr", odr_acc),
    ENUM_ROW(REG_ID_SENS_ACC_RANGE, "sens.acc_range", range_acc),
    ENUM_ROW(REG_ID_SENS_ACC_BWP, "sens.acc_bwp", bwp_acc),
    ENUM_ROW(REG_ID_SENS_ACC_PERF, "sens.acc_perf", perf),
    ENUM_ROW(REG_ID_SENS_GYR_ODR, "sens.gyr_odr", odr_gyr),
    ENUM_ROW(REG_ID_SENS_GYR_RANGE, "sens.gyr_range", range_gyr),
    ENUM_ROW(REG_ID_SENS_GYR_BWP, "sens.gyr_bwp", bwp_gyr),
    ENUM_ROW(REG_ID_SENS_GYR_PERF, "sens.gyr_perf", perf),
    ENUM_ROW(REG_ID_SENS_ENV_OS_T, "sens.env_os_t", oversample),
    ENUM_ROW(REG_ID_SENS_ENV_OS_P, "sens.env_os_p", oversample),
    ENUM_ROW(REG_ID_SENS_ENV_OS_H, "sens.env_os_h", oversample),
    ENUM_ROW(REG_ID_SENS_ENV_FILTER, "sens.env_filter", filter_size),
    {.id = REG_ID_SENS_HEAT_ON, .type = REG_T_BOOL, .flags = REG_F_RO, .name = "sens.heat_on", .get_applied = sens_get},
    {.id = REG_ID_SENS_HEAT_TEMP, .type = REG_T_U32, .flags = REG_F_RO, .name = "sens.heat_temp", .unit = "C",
     .get_applied = sens_get},
    {.id = REG_ID_SENS_HEAT_MS, .type = REG_T_U32, .flags = REG_F_RO, .name = "sens.heat_ms", .unit = "ms",
     .get_applied = sens_get},
};

reg_p4_dom_t const reg_p4_sensors = {.rows = rows, .count = sizeof(rows) / sizeof(rows[0])};
