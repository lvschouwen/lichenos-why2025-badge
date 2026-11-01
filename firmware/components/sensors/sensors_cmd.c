// `fw sensors`: one line per sensor; `fw sensors raw`: chip ids, status registers and the service's counters;
// `fw sensors start|stop`
#include "esp_timer.h"
#include "fw_console.h"
#include "lichen_sensors.h"
#include "sensors.h"
#include "sensors_chips.h"
#include "sensors_format.h"

#include <stdio.h>

#include <string.h>

// Why a reading is not valid, for the line that shows it
static char const *why(bool running, bool up, int err, uint32_t inits, char *buf, size_t cap) {
    if (!running) {
        return "stopped";
    }
    if (!inits || up) {
        return "waiting for the first sample";
    }
    if (err == 0) {
        return "stopped answering, re-initialising"; // the last init worked, the readings since did not
    }
    snprintf(buf, cap, err == -3 ? "not found" : "init failed (%d)", err);
    return buf;
}

static void print_lines(void) {
    lichen_sensors_t s;
    sensors_stats_t  st;
    lichen_sensors_get(&s);
    sensors_stats(&st);
    bool    run = lichen_sensors_running();
    int64_t now = esp_timer_get_time();
    char    line[160], reason[40];
    sensors_fmt_imu(line, sizeof line, &s, now, why(run, st.imu_up, st.imu_err, st.imu_inits, reason, sizeof reason));
    console_reply("%s", line);
    sensors_fmt_env(line, sizeof line, &s, now, why(run, st.env_up, st.env_err, st.env_inits, reason, sizeof reason));
    console_reply("%s", line);
    sensors_fmt_die(
        line,
        sizeof line,
        &s,
        now,
        run ? (st.die_up ? "waiting for the first sample" : "unavailable") : "stopped"
    );
    console_reply("%s", line);
}

// Register reads straight from the console task: each is one short transaction, the bus driver serialises them with
// the sampling task's
static void print_raw(void) {
    sensors_stats_t st;
    sensors_stats(&st);
    uint8_t                 r[17];
    i2c_master_dev_handle_t imu = sensors_imu_dev();
    if (!imu) {
        console_reply("imu 0x%02x: no device (init %d)", SENSORS_IMU_ADDR, st.imu_err);
    } else if (sensors_bus_read(imu, 0x00, r, 4) || sensors_bus_read(imu, 0x21, r + 4, 3)) {
        console_reply("imu 0x%02x: read failed", SENSORS_IMU_ADDR);
    } else {
        // 0x00 chip id (0x24), 0x02 error, 0x03 status (drdy acc 0x80, gyr 0x40), 0x21 internal status (1 = init
        // ok), 0x22/0x23 temperature (0x0000 = 23 C, 1/512 K per count)
        console_reply(
            "imu 0x%02x: id 0x%02x err 0x%02x status 0x%02x internal 0x%02x temp 0x%02x%02x, acc %d %d %d "
            "gyr %d %d %d",
            SENSORS_IMU_ADDR,
            r[0],
            r[2],
            r[3],
            r[4],
            r[6],
            r[5],
            st.imu_raw[0],
            st.imu_raw[1],
            st.imu_raw[2],
            st.imu_raw[3],
            st.imu_raw[4],
            st.imu_raw[5]
        );
    }
    i2c_master_dev_handle_t env = sensors_env_dev();
    if (!env) {
        console_reply("env 0x%02x: no device (init %d)", SENSORS_ENV_ADDR, st.env_err);
    } else if (sensors_bus_read(env, 0xd0, r, 1) || sensors_bus_read(env, 0xf0, r + 1, 1)) {
        console_reply("env 0x%02x: read failed", SENSORS_ENV_ADDR);
    } else {
        uint8_t id = r[0], variant = r[1];
        char    hex[17 * 3 + 1] = "";
        if (sensors_bus_read(env, 0x1d, r, sizeof r) == 0) { // field 0: status, pressure, temp, hum, gas ADCs
            for (size_t i = 0; i < sizeof r; i++) {
                snprintf(hex + i * 3, sizeof hex - i * 3, " %02x", r[i]);
            }
        }
        console_reply(
            "env 0x%02x: id 0x%02x variant 0x%02x field0%s",
            SENSORS_ENV_ADDR,
            id,
            variant,
            hex[0] ? hex : " (read failed)"
        );
    }
    console_reply(
        "service: %s, imu %lu ms, env %lu ms; samples %lu/%lu, fails %lu/%lu, inits %lu/%lu, stack free %lu B",
        lichen_sensors_running() ? "running" : "stopped",
        (unsigned long)st.imu_ms,
        (unsigned long)st.env_ms,
        (unsigned long)st.imu_samples,
        (unsigned long)st.env_samples,
        (unsigned long)st.imu_fails,
        (unsigned long)st.env_fails,
        (unsigned long)st.imu_inits,
        (unsigned long)st.env_inits,
        (unsigned long)st.stack_free
    );
}

static void cmd_sensors(char const *args) {
    if (args[0] == '\0') {
        print_lines();
    } else if (strcmp(args, "raw") == 0) {
        print_raw();
    } else if (strcmp(args, "start") == 0) {
        lichen_sensors_start();
        console_reply("sensors: started");
    } else if (strcmp(args, "stop") == 0) {
        lichen_sensors_stop();
        console_reply("sensors: stopped");
    } else {
        console_reply("sensors [raw|start|stop]");
    }
}

void sensors_register_console(void) {
    console_register("sensors", "sensors [raw|start|stop] (IMU, environment, P4 die temperature)", cmd_sensors);
}
