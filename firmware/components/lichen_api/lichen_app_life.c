// The apps' lifecycle records (#231) behind lichen_sys_life.h: app_life.c's transitions over one record per app slot,
// under a spinlock (a leaf: every caller takes nothing else while it holds it, and nothing in here blocks or logs
// inside it). Internal .bss (~0.3 KB): read and written from the OTA, power, console and LVGL tasks, so never PSRAM.
#include "lichen_sys_life.h"
#include "lichen_sys_app.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "screen_ids.h"

#include <string.h>

static char const TAG[] = "life";

_Static_assert(SCREEN_APP_MAX >= LICHEN_MAX_APPS, "every app has a screen id (screen_ids.h)");

static app_life_t   s_life[LICHEN_MAX_APPS]; // zeroed: REGISTERED (app_life_test pins it)
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_ms(void) {
    return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

static bool index_ok(size_t index) {
    return index < LICHEN_MAX_APPS && index < lichen_app_count();
}

static char const *app_label(size_t index) {
    lichen_app_t const *app = lichen_app_at(index);
    return app ? (app->id ? app->id : app->name) : "?";
}

void lichen_sys_life_started(size_t index) {
    if (!index_ok(index)) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    bool ok = app_life_started(&s_life[index]);
    portEXIT_CRITICAL(&s_mux);
    if (!ok) {
        ESP_LOGW(TAG, "%s: background start not recorded: the app is past registered", app_label(index));
    }
}

void lichen_sys_life_fault_crashes(size_t index) {
    if (!index_ok(index)) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    app_life_fault_crashes(&s_life[index]);
    portEXIT_CRITICAL(&s_mux);
}

void lichen_sys_life_screens(int const *ids, int depth) {
    size_t apps = lichen_app_count();
    for (size_t i = 0; i < apps && i < LICHEN_MAX_APPS; i++) {
        bool shown, on_top;
        app_life_view(ids, depth, 1 + (int)i, 1, SCREEN_APP_MAX, &shown, &on_top);
        portENTER_CRITICAL(&s_mux);
        bool changed = app_life_show(&s_life[i], shown, on_top);
        uint8_t state = s_life[i].state;
        portEXIT_CRITICAL(&s_mux);
        if (changed) {
            ESP_LOGD(TAG, "%s %s", app_label(i), app_life_state_name((life_state_t)state));
        }
    }
}

life_begin_t lichen_sys_life_stop_begin(size_t index, life_reason_t reason) {
    if (!index_ok(index)) {
        return LIFE_BEGIN_BAD;
    }
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_mux);
    life_begin_t got = app_life_stop_begin(&s_life[index], reason, now);
    portEXIT_CRITICAL(&s_mux);
    return got;
}

life_state_t lichen_sys_life_stop_end(size_t index, bool stopped, uint32_t tasks_left) {
    if (!index_ok(index)) {
        return LIFE_STATE_COUNT;
    }
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_mux);
    life_state_t got = app_life_stop_end(&s_life[index], stopped, now);
    if (got != LIFE_STATE_COUNT) {
        app_life_note_tasks(&s_life[index], tasks_left);
    }
    portEXIT_CRITICAL(&s_mux);
    return got;
}

life_wait_t lichen_sys_life_wait(size_t index) {
    if (!index_ok(index)) {
        return LIFE_WAIT_FAILED;
    }
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_mux);
    life_wait_t got = app_life_wait(&s_life[index], now);
    portEXIT_CRITICAL(&s_mux);
    return got;
}

bool lichen_sys_life_get(size_t index, app_life_t *out) {
    if (!index_ok(index)) {
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    *out = s_life[index];
    portEXIT_CRITICAL(&s_mux);
    return true;
}

void lichen_sys_life_console(void) {
    size_t apps = lichen_app_count();
    for (size_t i = 0; i < apps; i++) {
        app_life_t r;
        if (!lichen_sys_life_get(i, &r)) {
            break;
        }
        if (!r.stops) {
            console_reply("app life %s: %s (fault %s)", app_label(i), app_life_state_name((life_state_t)r.state),
                          app_life_fault_name((life_fault_t)r.fault));
            continue;
        }
        console_reply("app life %s: %s (fault %s); %u stop(s), the last for %s took %u ms, %u task(s) left",
                      app_label(i), app_life_state_name((life_state_t)r.state),
                      app_life_fault_name((life_fault_t)r.fault), (unsigned)r.stops,
                      app_life_reason_name((life_reason_t)r.reason), (unsigned)r.took_ms, (unsigned)r.tasks_left);
    }
    console_reply("app life: %u apps", (unsigned)apps);
}
