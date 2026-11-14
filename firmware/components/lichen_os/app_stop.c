// stop(reason) (#231, lichen_sys_life.h): what shell_stop_apps did since #76 (every app's stop_for_update inside its
// context, on the calling task, in registration order), now with a lifecycle record around each hook: the deadline,
// the verdict, the tasks it left. Cooperative: a hook is never interrupted and no task is ever deleted here; a late or
// refused stop is reported. The hooks' own behaviour and order are unchanged, so an update stops exactly as before.
//
// Lock order: this code holds no lock while a hook runs or while it waits for another caller's verdict. The records'
// spinlock (lichen_app_life.c) and the task book's are leaves, taken only for a copy. A hook may take the mesh run lock,
// the mesh lock or the LVGL lock (through lichen_ui_call): stop never holds any of them, so a hook that waits on one
// cannot deadlock against stop, only overrun its deadline.
#include "lichen_sys_life.h"
#include "lichen_sys_app.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_os.h"

static char const TAG[] = "update"; // shell_stop_apps's tag: the update log reads as before

#define WAIT_POLL_MS 50

// Another caller is stopping the app (the update's stop, then the restart's save on its own task): its verdict, waited
// for up to the deadline. A hook that hangs past it is reported, and this caller goes on.
static bool wait_other(size_t index, lichen_app_t const *app, life_reason_t reason) {
    for (;;) {
        switch (lichen_sys_life_wait(index)) {
        case LIFE_WAIT_PENDING:
            vTaskDelay(pdMS_TO_TICKS(WAIT_POLL_MS));
            continue;
        case LIFE_WAIT_STOPPED:
            return true;
        case LIFE_WAIT_OVERDUE:
            ESP_LOGW(TAG, "%s: still stopping, past its deadline: not waited for any longer (%s)", app->name,
                     app_life_reason_name(reason));
            return false;
        case LIFE_WAIT_FAILED:
        default:
            ESP_LOGE(TAG, "%s is still running", app->name);
            return false;
        }
    }
}

static void report(lichen_app_t const *app, life_reason_t reason, size_t index, size_t tasks_left) {
    app_life_t r;
    if (!lichen_sys_life_get(index, &r)) {
        return;
    }
    if (r.state == LIFE_FAULTED && r.fault == LIFE_FAULT_STOP_LATE) {
        ESP_LOGW(TAG, "%s stopped for the %s after %u ms, past its %lu ms deadline", app->name,
                 app_life_reason_name(reason), (unsigned)r.took_ms, (unsigned long)app_life_deadline_ms(reason));
    }
    if (tasks_left) {
        // Counted, never deleted: a task the app started ends when the app ends it (lichen_task.h)
        ESP_LOGI(TAG, "%s: %u of its tasks still run after its stop", app->name, (unsigned)tasks_left);
    }
}

static bool stop_one(size_t index, life_reason_t reason) {
    lichen_app_t const *app = lichen_app_at(index);
    if (!app) {
        return true;
    }
    lichen_ctx_t ctx = lichen_sys_app_ctx_at(index);
    switch (lichen_sys_life_stop_begin(index, reason)) {
    case LIFE_BEGIN_BUSY:
        return wait_other(index, app, reason);
    case LIFE_BEGIN_BAD:
        return false;
    case LIFE_BEGIN_OK:
    default:
        break;
    }
    bool stopped = true; // an app without a hook has nothing running that a stop would end
    if (app->stop_for_update) {
        lichen_ctx_t previous = lichen_ctx_enter(ctx); // #230: on the stopping task, for the app
        stopped               = app->stop_for_update();
        lichen_ctx_leave(previous);
    }
    if (!stopped) {
        ESP_LOGE(TAG, "%s is still running", app->name);
    }
    size_t tasks_left = lichen_os_task_count(ctx);
    lichen_sys_life_stop_end(index, stopped, (uint32_t)tasks_left);
    report(app, reason, index, tasks_left);
    return stopped;
}

bool lichen_sys_apps_stop(life_reason_t reason) {
    bool all_stopped = true;
    for (size_t i = 0; i < lichen_app_count(); i++) {
        if (!stop_one(i, reason)) {
            all_stopped = false;
        }
    }
    return all_stopped;
}
