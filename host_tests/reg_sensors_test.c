// Host test: the sens.* registry keys (#318): table consistency, the chip codes the firmware configures read back as
// the documented names, the BME690 register decoding, and a missing chip reading as a failure, not zeros
#include "reg.h"
#include "reg_p4.h"
#include "sensors_cfg.h"
#include "ui_service.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// Test doubles for what the sensors component and lichen_os provide (this test links no stub file)
static sensors_cfg_t cfg;
static bool          last_live;
static bool          lvgl_task;

ui_lock_verdict_t ui_service_lock_check(void) {
    return lvgl_task ? UI_LOCK_OK : UI_LOCK_UNARMED;
}

bool sensors_cfg_get(unsigned key, bool live, uint32_t *out) {
    last_live = live;
    return sensors_cfg_value(&cfg, key, out);
}

static char const *show(char const *name, char *buf, size_t cap) {
    reg_entry_t const *e = reg_find_name(reg_p4_sensors.rows, reg_p4_sensors.count, name);
    reg_value_t        v;
    if (!e || reg_get(e, &v) != REG_OK) {
        return NULL;
    }
    buf[reg_format(e, &v, buf, cap)] = '\0';
    return buf;
}

static void test_table(void) {
    CHECK(reg_p4_sensors.count == SENSORS_CFG_KEYS);
    CHECK(reg_table_check(reg_p4_sensors.rows, reg_p4_sensors.count) == reg_p4_sensors.count);
    for (size_t i = 0; i < reg_p4_sensors.count; i++) {
        reg_entry_t const *e = &reg_p4_sensors.rows[i];
        CHECK(REG_ID_DOMAIN(e->id) == 0x13);
        CHECK((e->flags & REG_F_RO) && !e->set && !e->saved_key);
        CHECK((e->id & 0xFFu) == i); // the low byte is sensors_cfg_key_t
        CHECK(strncmp(e->name, "sens.", 5) == 0);
    }
    CHECK(REG_ID_SENS_HEAT_MS == 0x1300 + SENSORS_CFG_HEAT_MS);
    CHECK(REG_ID_SENS_ENV_FILTER == 0x1300 + SENSORS_CFG_ENV_FILTER);
}

static void test_missing_chip(void) {
    char b[32];
    memset(&cfg, 0, sizeof cfg); // neither chip read: every key fails, none reads as 0 / "reserved"
    for (size_t i = 0; i < reg_p4_sensors.count; i++) {
        reg_value_t v;
        CHECK(reg_get(&reg_p4_sensors.rows[i], &v) == REG_FAILED);
    }
    cfg.imu_ok = true; // the IMU alone: the BME690 keys still fail
    CHECK(show("sens.acc_odr", b, sizeof b) != NULL);
    CHECK(show("sens.env_os_t", b, sizeof b) == NULL);
    CHECK(show("sens.heat_ms", b, sizeof b) == NULL);
}

// What sensors_imu.c and sensors_env.c configure: 100 Hz, +-4 g, +-1000 dps; BME 2x T, 4x P, 1x H, no filter, 300 C
static void test_firmware_config(void) {
    char b[32];
    cfg = (sensors_cfg_t){.imu_ok = true, .env_ok = true, .acc_odr = 8, .acc_range = 1, .acc_bwp = 2, .acc_perf = 1,
                          .gyr_odr = 8, .gyr_range = 1, .gyr_bwp = 2, .gyr_perf = 1, .env_os_t = 2, .env_os_p = 3,
                          .env_os_h = 1, .env_filter = 0, .heat_on = true, .heat_c = 300, .heat_ms = 100};
    CHECK(strcmp(show("sens.acc_odr", b, sizeof b), "100Hz") == 0);
    CHECK(strcmp(show("sens.acc_range", b, sizeof b), "4g") == 0);
    CHECK(strcmp(show("sens.acc_bwp", b, sizeof b), "normal_avg4") == 0);
    CHECK(strcmp(show("sens.acc_perf", b, sizeof b), "perf") == 0);
    CHECK(strcmp(show("sens.gyr_odr", b, sizeof b), "100Hz") == 0);
    CHECK(strcmp(show("sens.gyr_range", b, sizeof b), "1000dps") == 0);
    CHECK(strcmp(show("sens.gyr_bwp", b, sizeof b), "normal") == 0);
    CHECK(strcmp(show("sens.env_os_t", b, sizeof b), "2x") == 0);
    CHECK(strcmp(show("sens.env_os_p", b, sizeof b), "4x") == 0);
    CHECK(strcmp(show("sens.env_os_h", b, sizeof b), "1x") == 0);
    CHECK(strcmp(show("sens.env_filter", b, sizeof b), "off") == 0);
    CHECK(strcmp(show("sens.heat_on", b, sizeof b), "on") == 0);
    CHECK(strcmp(show("sens.heat_temp", b, sizeof b), "300") == 0);
    CHECK(strcmp(show("sens.heat_ms", b, sizeof b), "100") == 0);
    cfg.acc_odr = 0; // a reserved code the table names, a code beyond it fails
    CHECK(strcmp(show("sens.acc_odr", b, sizeof b), "reserved") == 0);
    cfg.acc_odr = 13;
    CHECK(show("sens.acc_odr", b, sizeof b) == NULL);
    cfg.gyr_odr = 13;
    CHECK(strcmp(show("sens.gyr_odr", b, sizeof b), "3200Hz") == 0);
    cfg.gyr_odr = 14;
    CHECK(show("sens.gyr_odr", b, sizeof b) == NULL);
}

static void test_live_only_off_lvgl(void) {
    char b[32];
    lvgl_task = false;
    show("sens.acc_odr", b, sizeof b);
    CHECK(last_live); // the console asks the chip
    lvgl_task = true;
    show("sens.acc_odr", b, sizeof b);
    CHECK(!last_live); // the LVGL task reads the cache
    lvgl_task = false;
}

// Bosch's calc_res_heat (float variant, bme69x.c), copied here to round-trip the inverse
static uint8_t res_heat_of(int t, int8_t g1, int16_t g2, int8_t g3, uint8_t rhr, int8_t rhv, int amb) {
    float var1 = ((float)g1 / 16.0f) + 49.0f;
    float var2 = (((float)g2 / 32768.0f) * 0.0005f) + 0.00235f;
    float var3 = (float)g3 / 1024.0f;
    float var4 = var1 * (1.0f + (var2 * (float)t));
    float var5 = var4 + (var3 * (float)amb);
    return (uint8_t)(3.4f * ((var5 * (4 / (4 + (float)rhr)) * (1 / (1 + ((float)rhv * 0.002f)))) - 25));
}

static void test_decode(void) {
    CHECK(sensors_heat_ms(0x40 | 25) == 100);
    CHECK(sensors_heat_ms(63) == 63);
    CHECK(sensors_heat_ms(0x80 | 10) == 160);
    CHECK(sensors_heat_ms(0xC0 | 63) == 63u * 64u);
    CHECK(sensors_heat_ms(0) == 0);
    CHECK(sensors_heat_on(0x00, 0x20));  // heater on, run_gas = 1
    CHECK(!sensors_heat_on(0x08, 0x20)); // heat off bit
    CHECK(!sensors_heat_on(0x00, 0x00)); // gas measurement off
    int8_t  g1 = -5, g3 = 18, rhv = 40;
    int16_t g2 = -2400;
    for (int t = 200; t <= 400; t += 25) {
        uint8_t r   = res_heat_of(t, g1, g2, g3, 1, rhv, 25);
        sensors_heat_cal_t cal = {.par_g1 = g1, .par_g2 = g2, .par_g3 = g3, .res_heat_range = 1, .res_heat_val = rhv};
        int     got = (int)sensors_heat_temp_c(r, &cal, 25);
        CHECK(abs(got - t) <= 4); // one register step is about 2 C here
    }
    sensors_heat_cal_t cal = {.par_g1 = g1, .par_g2 = g2, .par_g3 = g3, .res_heat_range = 1, .res_heat_val = rhv};
    CHECK(sensors_heat_temp_c(0, &cal, 25) <= 400);
}

int main(void) {
    test_table();
    test_missing_chip();
    test_firmware_config();
    test_live_only_off_lvgl();
    test_decode();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("reg_sensors_test: ok\n");
    return 0;
}
