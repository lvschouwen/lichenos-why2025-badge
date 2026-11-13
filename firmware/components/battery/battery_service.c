// The battery service (#11, battery.h). One task: probe the camera header's bus once, then poll the MAX17048 every
// BAT_POLL_MS while it answers (BAT_LOST_POLL_MS once it stopped), and hand each poll to battery_svc.c. With no gauge
// the bus is closed again and the task blocks until "fw battery probe": no polling, no retries, the pins left as inputs.
#include "battery.h"

#include "board_i2c.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "max17048.h"

#include <stdatomic.h>
#include <string.h>

static char const TAG[] = "battery";

// The automatic 3.30 V power-off stays off until the readings are calibrated against a meter (#11): with it false the
// service cannot ask for a power-off whatever the chip reports, and nothing in this build could act on one anyway.
#define BATTERY_AUTO_OFF false
#define TMP102_ADDR      0x48
#define PROBE_MS         20 // per transfer: an unanswered address NACKs at once, a stalled one hits the 20 ms timeout

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static bat_svc_t      svc;  // the task's own
static battery_snap_t                  snap; // under lock: internal RAM, a critical section never touches PSRAM (#271)
static _Atomic(TaskHandle_t)           task; // set by the task itself, read by the console
static atomic_bool                     probe_asked = true; // the task's first round probes
static bool                            tmp102_seen, off_logged, stack_logged;
static uint32_t                        probe_ms;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// 0x36 first, then 0x48. A MAX17048 is left attached. The first transfer that stalls ends the probe, so a stuck bus
// costs one 20 ms timeout (and the one error line IDF logs for it); every other step NACKs or answers in well under a
// millisecond at 100 kHz, so a probe takes about 1 ms normally and about 25 ms at worst.
static bat_probe_t look(i2c_master_bus_handle_t bus) {
    esp_err_t err = i2c_master_probe(bus, MAX17048_ADDR, PROBE_MS);
    if (err == ESP_ERR_TIMEOUT) {
        return BAT_PROBE_TIMEOUT;
    }
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return BAT_PROBE_BUS_ERROR;
    }
    bat_probe_t result = BAT_PROBE_NOTHING;
    if (err == ESP_OK) {
        uint16_t version = 0;
        if (max17048_attach(bus) != ESP_OK) {
            return BAT_PROBE_BUS_ERROR;
        }
        if (!max17048_read_version(&version)) {
            return BAT_PROBE_TIMEOUT; // it acknowledged its address, then the read failed
        }
        result = bat_version_valid(version) ? BAT_PROBE_FOUND : BAT_PROBE_NOT_MAX;
    }
    tmp102_seen = i2c_master_probe(bus, TMP102_ADDR, PROBE_MS) == ESP_OK; // mod 5; nothing reads it yet
    return result;
}

static void probe(void) {
    int64_t t0 = esp_timer_get_time();
    max17048_detach(); // a re-probe starts from a closed bus
    board_i2c2_close();
    tmp102_seen                 = false;
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t               err = board_i2c2_open(&bus);
    bat_probe_t             result;
    if (err == ESP_ERR_INVALID_RESPONSE) {
        result = BAT_PROBE_LINES_LOW; // nothing was sent
    } else if (err != ESP_OK) {
        result = BAT_PROBE_BUS_ERROR;
    } else {
        result = look(bus);
    }
    if (result != BAT_PROBE_FOUND) {
        max17048_detach();
        board_i2c2_close(); // the camera header's pins back to plain inputs
    }
    battery_service_probed(&svc, result);
    off_logged = false;
    probe_ms   = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (result == BAT_PROBE_FOUND) {
        ESP_LOGI(TAG, "MAX17048 on I2C2 (SDA %d, SCL %d)%s", BOARD_I2C2_SDA, BOARD_I2C2_SCL,
                 tmp102_seen ? ", TMP102 at 0x48 too" : "");
    } else {
        ESP_LOGI(TAG, "no fuel gauge on I2C2 (SDA %d, SCL %d): %s (%u ms)", BOARD_I2C2_SDA, BOARD_I2C2_SCL,
                 battery_probe_name(result), (unsigned)probe_ms);
    }
}

static void gauge_changed(bat_gauge_t before, bat_gauge_t after, bat_regs_verdict_t why) {
    if (after == before) {
        return;
    }
    if (after == BAT_GAUGE_STALE && before == BAT_GAUGE_PRESENT) {
        ESP_LOGW(TAG, "gauge read failed or implausible (%s); keeping the last reading",
                 battery_regs_verdict_name(why));
    } else if (after == BAT_GAUGE_LOST) {
        ESP_LOGW(TAG, "gauge lost after %d failed polls; trying once a minute", BAT_LOST_FAILS);
    } else if (after == BAT_GAUGE_PRESENT) {
        ESP_LOGI(TAG, "gauge readings back");
    }
}

static void acted(bat_action_t act) {
    if (act == BAT_ACT_WARN) {
        ESP_LOGW(TAG, "battery low: %u mV", (unsigned)svc.mv);
    } else if (act == BAT_ACT_WARN_CLEAR) {
        ESP_LOGI(TAG, "battery no longer low: %u mV", (unsigned)svc.mv);
    } else if (act == BAT_ACT_OFF) {
        ESP_LOGE(TAG, "power-off asked for, but no power-off path is wired in this build"); // unreachable: see above
    }
    if (svc.off_due && !off_logged) {
        off_logged = true;
        ESP_LOGW(TAG, "cell at or below %d mV for %d s; automatic power-off is disabled until calibrated", BAT_OFF_MV,
                 BAT_DWELL_MS / 1000);
    }
}

static void poll_once(void) {
    bat_regs_t r;
    max17048_read(&r);
    bat_gauge_t  before = svc.gauge;
    bat_action_t act    = battery_service_step(&svc, &r, BATTERY_AUTO_OFF, now_ms());
    gauge_changed(before, svc.gauge, battery_regs_check(&r));
    acted(act);
}

static void publish(void) {
    uint32_t free_b = (uint32_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
    if (!stack_logged) {
        stack_logged = true;
        ESP_LOGI(TAG, "task stack: %u of %d B never used", (unsigned)free_b, BATTERY_STACK_BYTES);
    }
    battery_snap_t s = {
        .gauge            = svc.gauge,
        .probe            = svc.probe,
        .have             = svc.have,
        .mv               = svc.mv,
        .percent          = (uint8_t)((svc.soc_pct256 + 128u) >> 8),
        .curve_percent    = svc.have ? bat_mv_to_percent((int32_t)svc.mv) : 0,
        .crate_centipct_h = svc.crate_centipct_h,
        .trend            = svc.have ? svc.trend : BAT_TREND_UNKNOWN,
        .level            = svc.model.level,
        .off_due          = svc.off_due,
        .age_ms           = svc.have ? now_ms() - svc.last_ok_ms : 0,
        .tmp102_seen      = tmp102_seen,
        .polls            = svc.polls,
        .io_errors        = svc.io_errors,
        .rejected         = svc.rejected,
        .last_reject      = svc.last_reject,
        .probe_ms         = probe_ms,
        .stack_free       = free_b,
    };
    taskENTER_CRITICAL(&lock);
    snap = s;
    taskEXIT_CRITICAL(&lock);
}

void battery_service_run(void) {
    atomic_store(&task, xTaskGetCurrentTaskHandle());
    battery_service_init(&svc);
    for (;;) {
        if (atomic_exchange(&probe_asked, false)) {
            probe();
        }
        if (battery_service_poll_ms(&svc)) {
            poll_once();
        }
        publish();
        uint32_t period = battery_service_poll_ms(&svc);
        // Sleeps the whole period (absent: until a probe is asked for); a probe request wakes it early
        ulTaskNotifyTake(pdTRUE, period ? pdMS_TO_TICKS(period) : portMAX_DELAY);
    }
}

void battery_get(battery_snap_t *out) {
    taskENTER_CRITICAL(&lock);
    *out = snap;
    taskEXIT_CRITICAL(&lock);
}

bool battery_service_reprobe(void) {
    TaskHandle_t t = atomic_load(&task);
    if (!t) {
        return false;
    }
    atomic_store(&probe_asked, true);
    xTaskNotifyGive(t);
    return true;
}
