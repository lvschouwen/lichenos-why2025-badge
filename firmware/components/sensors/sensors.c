// The sensors service (#69): one task on core 0 owns the BMI270, the BME690 and the P4 temperature sensor, samples
// them at low rates and publishes a snapshot under a spinlock. A missing chip costs one warning at boot and a probe
// every 30 s; a chip that stops answering is re-initialised. Nothing here blocks boot or holds the bus for long.
#include "sensors.h"

#include "board_i2c.h"
#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_sensors.h"
#include "sdkconfig.h"
#include "sensors_chips.h"
#include "sensors_format.h"

#include <stdatomic.h>

static char const TAG[] = "sensors";

#define STACK_BYTES   6144 // the Bosch calls' depth is not measured yet: the margin is logged once (see log_stack)
#define PRIORITY      2    // below the keyboard (4): a key press never waits for a sample
#define RETRY_US      (30 * 1000000LL)
#define IMU_FAILS     5 // in a row, then the chip counts as gone and is re-initialised
#define ENV_FAILS     3
#define IMU_STALE_US  1000000LL // no new IMU data for this long counts as a failed read (chip reset, stuck)
#define ENV_MARGIN_US 5000      // after the measurement time the BME690 reports, before the first read
#define ENV_RETRY_US  5000      // a measurement not done yet: read again this much later ...
#define ENV_RETRIES   4         // ... this often (about 20 ms) before it counts as a failure

typedef struct {
    char const *name;
    uint8_t     addr;
    bool        up;
    bool        warned; // one warning per outage, not one per retry
    int         fails;
    int64_t     retry_us;
} chip_t;

static portMUX_TYPE     lock = portMUX_INITIALIZER_UNLOCKED;
static lichen_sensors_t snap;  // under lock
static sensors_stats_t  stats; // under lock

static TaskHandle_t                task;
static atomic_bool                 created;
static atomic_bool                 want_run;
static _Atomic uint32_t            imu_ms = CONFIG_LICHEN_SENSORS_IMU_PERIOD_MS;
static _Atomic uint32_t            env_ms = CONFIG_LICHEN_SENSORS_ENV_PERIOD_MS;
static chip_t                      imu    = {.name = "BMI270", .addr = SENSORS_IMU_ADDR};
static chip_t                      env    = {.name = "BME690", .addr = SENSORS_ENV_ADDR};
static temperature_sensor_handle_t tsens;
static bool                        die_enabled; // tsens installed and enabled
static int64_t                     imu_last_us; // the last new IMU data (or the init), for the staleness check
static int                         env_tries;   // reads of the running measurement that found no new data
static bool                        stack_logged;

static void chip_init(chip_t *c, i2c_master_bus_handle_t bus, int64_t now) {
    int  err = 0;
    bool ok  = c == &imu ? sensors_imu_init(bus, &err) : sensors_env_init(bus, &err);
    taskENTER_CRITICAL(&lock);
    if (c == &imu) {
        stats.imu_inits++, stats.imu_err = err, stats.imu_up = ok;
    } else {
        stats.env_inits++, stats.env_err = err, stats.env_up = ok;
    }
    taskEXIT_CRITICAL(&lock);
    c->fails = 0;
    c->up    = ok;
    if (c == &imu) {
        imu_last_us = esp_timer_get_time();
    }
    if (ok) {
        if (c->warned) {
            ESP_LOGW(TAG, "%s (0x%02x) is back", c->name, c->addr);
        }
        c->warned = false;
        return;
    }
    c->retry_us = now + RETRY_US;
    if (!c->warned) {
        ESP_LOGW(
            TAG,
            "%s (0x%02x): %s (%d), retrying every %d s",
            c->name,
            c->addr,
            err == -3 ? "not found" : "init failed",
            err,
            (int)(RETRY_US / 1000000)
        );
        c->warned = true;
    }
}

// A failed or implausible reading; enough of them in a row and the chip is re-initialised on the next pass
static void chip_failed(chip_t *c, int limit, int64_t now) {
    taskENTER_CRITICAL(&lock);
    if (c == &imu) {
        stats.imu_fails++;
    } else {
        stats.env_fails++;
    }
    bool down = ++c->fails >= limit;
    if (down) {
        if (c == &imu) {
            snap.imu_valid = false, stats.imu_up = false;
        } else {
            snap.env_valid = snap.gas_valid = false, stats.env_up = false;
        }
    }
    taskEXIT_CRITICAL(&lock);
    if (down) {
        ESP_LOGW(TAG, "%s (0x%02x) stopped answering, re-initialising", c->name, c->addr);
        c->up       = false;
        c->warned   = true;
        c->retry_us = now;
    }
}

static void sample_imu(int64_t now) {
    float          a[3], g[3];
    int16_t        raw[6];
    sensors_read_t r = sensors_imu_read(a, g, raw);
    if (r == SENSORS_READ_NONE) {
        // Read before the next output sample: try again next pass. Only a long silence counts (a chip that was reset
        // stops producing data but still answers).
        if (now - imu_last_us > IMU_STALE_US) {
            imu_last_us = now;
            chip_failed(&imu, IMU_FAILS, now);
        }
        return;
    }
    if (r != SENSORS_READ_OK || !sensors_imu_plausible(a, SENSORS_IMU_RANGE_G, g, SENSORS_IMU_RANGE_DPS)) {
        chip_failed(&imu, IMU_FAILS, now);
        return;
    }
    imu.fails   = 0;
    imu_last_us = now;
    taskENTER_CRITICAL(&lock);
    snap.imu_valid = true;
    snap.imu_us    = now;
    for (int i = 0; i < 3; i++) {
        snap.accel_g[i]  = a[i];
        snap.gyro_dps[i] = g[i];
    }
    for (int i = 0; i < 6; i++) {
        stats.imu_raw[i] = raw[i];
    }
    stats.imu_samples++;
    taskEXIT_CRITICAL(&lock);
}

// Logged once, at warning level so it shows: the stack headroom after both chips' init and the first environment
// sample, the deepest calls the task makes (#69 review: STACK_BYTES can shrink once bench logs show the margin)
static void log_stack(void) {
    if (!stack_logged) {
        stack_logged = true;
        ESP_LOGW(
            TAG,
            "task stack: %u of %d B never used",
            (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
            STACK_BYTES
        );
    }
}

// False while the measurement is still running and worth another read in ENV_RETRY_US
static bool sample_env(int64_t now) {
    float          t, rh, hpa, kohm;
    bool           gas_ok = false;
    sensors_read_t r      = sensors_env_read(&t, &rh, &hpa, &kohm, &gas_ok);
    if (r == SENSORS_READ_NONE && ++env_tries < ENV_RETRIES) {
        return false;
    }
    env_tries = 0;
    if (r != SENSORS_READ_OK || !sensors_env_plausible(t, rh, hpa)) {
        chip_failed(&env, ENV_FAILS, now);
        return true;
    }
    env.fails = 0;
    taskENTER_CRITICAL(&lock);
    snap.env_valid     = true;
    snap.env_us        = now;
    snap.temperature_c = t;
    snap.humidity_pct  = rh;
    snap.pressure_hpa  = hpa;
    snap.gas_valid     = gas_ok;
    snap.gas_kohm      = gas_ok ? kohm : 0.0f;
    stats.env_samples++;
    taskEXIT_CRITICAL(&lock);
    log_stack();
    return true;
}

static void sample_die(int64_t now) {
    float c     = 0.0f;
    bool  valid = die_enabled && temperature_sensor_get_celsius(tsens, &c) == ESP_OK && sensors_die_plausible(c);
    taskENTER_CRITICAL(&lock);
    snap.die_valid = valid;
    if (valid) {
        snap.die_c  = c;
        snap.die_us = now;
    }
    taskEXIT_CRITICAL(&lock);
}

// Stop: accel and gyro off (the BME690 sleeps between forced measurements anyway), the die sensor off, and nothing
// valid any more. Start: the reverse; a chip that is not up is initialised by the loop.
static void set_power(bool on) {
    if (imu.up && !sensors_imu_power(on)) {
        imu.up       = false;
        imu.retry_us = 0;
    }
    bool die_on = false;
    if (tsens) {
        esp_err_t err = on ? temperature_sensor_enable(tsens) : temperature_sensor_disable(tsens);
        die_on        = on && err == ESP_OK;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "P4 temperature sensor %s: %s", on ? "enable" : "disable", esp_err_to_name(err));
        }
    }
    taskENTER_CRITICAL(&lock);
    if (!on) {
        snap.imu_valid = snap.env_valid = snap.gas_valid = false;
    }
    if (!die_on) {
        snap.die_valid = false;
    }
    stats.die_up = die_on;
    taskEXIT_CRITICAL(&lock);
    die_enabled = die_on;
}

static int64_t earliest(int64_t a, int64_t b) {
    return a < b ? a : b;
}

static void sensors_task(void *arg) {
    (void)arg;
    i2c_master_bus_handle_t     bus = board_i2c_bus(); // the OS created it; never a bus of our own (#87)
    temperature_sensor_config_t tc  = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tc, &tsens) != ESP_OK) {
        ESP_LOGW(TAG, "P4 temperature sensor not available");
        tsens = NULL;
    }
    bool    running  = false;
    int64_t imu_next = 0, env_next = 0, env_read_at = 0, die_next = 0; // env_read_at: 0 = no measurement running
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (atomic_load(&want_run) != running) {
            running = !running;
            set_power(running);
            imu_next = env_next = die_next = now;
            imu_last_us                    = now; // a long stop is not a silent chip
            env_read_at                    = 0;
        }
        if (running) {
            if (!imu.up && now >= imu.retry_us) {
                chip_init(&imu, bus, now);
            }
            if (!env.up && now >= env.retry_us) {
                chip_init(&env, bus, now);
                env_read_at = 0;
            }
            now = esp_timer_get_time(); // an init takes a while (the BMI270's config upload)
            if (imu.up && now >= imu_next) {
                sample_imu(now);
                imu_next = now + (int64_t)atomic_load(&imu_ms) * 1000;
            }
            if (env.up && env_read_at && now >= env_read_at) {
                env_read_at = sample_env(now) ? 0 : now + ENV_RETRY_US;
            }
            if (env.up && !env_read_at && now >= env_next) {
                uint32_t dur_us = sensors_env_trigger(); // the heater runs while the bus is free
                if (dur_us) {
                    env_read_at = now + dur_us + ENV_MARGIN_US;
                    env_tries   = 0;
                } else {
                    chip_failed(&env, ENV_FAILS, now);
                }
                env_next = now + (int64_t)atomic_load(&env_ms) * 1000;
            }
            if (now >= die_next) {
                sample_die(now);
                die_next = now + (int64_t)atomic_load(&env_ms) * 1000;
            }
        }
        TickType_t wait = portMAX_DELAY;
        if (running) {
            int64_t wake = earliest(die_next, imu.up ? imu_next : imu.retry_us);
            wake         = earliest(wake, env.up ? (env_read_at ? env_read_at : env_next) : env.retry_us);
            int64_t ms   = (wake - esp_timer_get_time() + 999) / 1000;
            wait         = pdMS_TO_TICKS(ms > 0 ? ms : 0);
            wait         = wait ? wait : 1;
        }
        ulTaskNotifyTake(pdTRUE, wait); // start, stop and new periods wake it early
        taskENTER_CRITICAL(&lock);
        stats.stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
        taskEXIT_CRITICAL(&lock);
    }
}

void lichen_sensors_get(lichen_sensors_t *out) {
    taskENTER_CRITICAL(&lock);
    *out = snap;
    taskEXIT_CRITICAL(&lock);
}

static void poke(void) {
    if (task) {
        xTaskNotifyGive(task);
    }
}

void lichen_sensors_start(void) {
    atomic_store(&want_run, true);
    bool expected = false;
    if (!atomic_compare_exchange_strong(&created, &expected, true)) {
        poke();
        return;
    }
    // Internal RAM stack: a PSRAM stack would stall whenever flash is written (cache off)
    if (xTaskCreatePinnedToCore(sensors_task, "sensors", STACK_BYTES, NULL, PRIORITY, &task, 0) != pdPASS) {
        ESP_LOGW(TAG, "sensors task not started");
        atomic_store(&created, false);
        atomic_store(&want_run, false);
    }
}

void lichen_sensors_stop(void) {
    atomic_store(&want_run, false);
    poke();
}

bool lichen_sensors_running(void) {
    return task && atomic_load(&want_run);
}

void lichen_sensors_set_periods(uint32_t imu_req, uint32_t env_req) {
    atomic_store(&imu_ms, sensors_period(imu_req, atomic_load(&imu_ms), SENSORS_IMU_MIN_MS, SENSORS_IMU_MAX_MS));
    atomic_store(&env_ms, sensors_period(env_req, atomic_load(&env_ms), SENSORS_ENV_MIN_MS, SENSORS_ENV_MAX_MS));
    poke();
}

void sensors_stats(sensors_stats_t *out) {
    taskENTER_CRITICAL(&lock);
    *out = stats;
    taskEXIT_CRITICAL(&lock);
    out->imu_ms = atomic_load(&imu_ms);
    out->env_ms = atomic_load(&env_ms);
}
