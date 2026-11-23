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
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_mem.h"
#include "lichen_os.h"
#include "lichen_sys_ui.h"

static char const TAG[] = "update"; // shell_stop_apps's tag: the update log reads as before

#define WAIT_POLL_MS 50
#define TIMER_DROP_LOCK_MS 1000 // the LVGL lock, only to queue the drop; the badge goes down after a stop anyway

static void drop_timers(void *arg) {
    lichen_sys_timer_drop((uint32_t)(uintptr_t)arg);
}

// The app's booked timers go with its stop: dead at once (any later tick deletes itself), deleted on the LVGL task
// when its lock is free. Queued, never run here: this task holds no LVGL lock. Killed even with none booked: a task the
// hook left running must not book one after the stop. Only the drop, which needs the lock, waits for a booked timer.
static void stop_timers(char const *name, lichen_ctx_t ctx) {
    lichen_sys_timer_kill(ctx);
    if (!lichen_sys_timer_count(ctx)) {
        return;
    }
    if (!lichen_sys_ui_call(drop_timers, (void *)(uintptr_t)ctx, TIMER_DROP_LOCK_MS)) {
        ESP_LOGW(TAG, "%s: booked timers left to end at their next tick (LVGL busy)", name);
    }
}

// Another caller is stopping the app (the update's stop, then the restart's save on its own task): its verdict, waited
// for up to this caller's own reason's deadline from now. A hook that hangs past it is reported, and this caller goes
// on; a late hook that did stop counts as stopped (app_life_wait_verdict).
static bool wait_other(lichen_ctx_t ctx, char const *name, life_reason_t reason) {
    uint32_t since = lichen_sys_life_now_ms();
    for (;;) {
        switch (lichen_sys_life_wait(ctx, reason, since)) {
        case LIFE_WAIT_PENDING:
            vTaskDelay(pdMS_TO_TICKS(WAIT_POLL_MS));
            continue;
        case LIFE_WAIT_STOPPED:
            return true;
        case LIFE_WAIT_OVERDUE:
            ESP_LOGW(TAG, "%s: still stopping, not waited for longer", name);
            return false;
        case LIFE_WAIT_FAILED:
        default:
            ESP_LOGE(TAG, "%s is still running", name);
            return false;
        }
    }
}

// One line per app that stopped late or left something; nothing for one that stopped clean. Counted, never deleted or
// freed: a task the app started ends when the app ends it, and a block may still be in use by it.
static void report(char const *name, lichen_ctx_t ctx, size_t tasks_left) {
    app_life_t r;
    uint32_t   internal = 0, bulk = 0;
    lichen_os_mem_held(ctx, &internal, &bulk);
    if (!lichen_sys_life_get(ctx, &r)) {
        return;
    }
    bool late = r.state == LIFE_FAULTED && r.fault == LIFE_FAULT_STOP_LATE;
    if (late || tasks_left || internal || bulk) {
        ESP_LOGI(TAG, "%s stopped%s in %u ms: %u tasks run, %lu B internal, %lu B PSRAM held", name,
                 late ? " late" : "", (unsigned)r.took_ms, (unsigned)tasks_left, (unsigned long)internal,
                 (unsigned long)bulk);
    }
}

// Stops the app of ctx; one that has left since the loop began (a removal mid-stop) is stopped already
static bool stop_one(lichen_ctx_t ctx, life_reason_t reason) {
    lichen_app_t const *app = lichen_sys_app_of(ctx);
    if (!app) {
        return true;
    }
    // Everything of the app struct that is needed after the stop is read now: once stop_end says STOPPED or FAULTED the
    // app may be unregistered and its struct freed (#520)
    char name[32];
    strlcpy(name, app->name ? app->name : "?", sizeof(name));
    bool (*hook)(void) = app->stop_for_update;
    switch (lichen_sys_life_stop_begin(ctx, reason)) {
    case LIFE_BEGIN_BUSY:
        return wait_other(ctx, name, reason);
    case LIFE_BEGIN_BAD:
        return false;
    case LIFE_BEGIN_OK:
    default:
        break;
    }
    bool stopped = true; // an app without a hook has nothing running that a stop would end
    if (hook) {
        lichen_ctx_t previous = lichen_ctx_enter(ctx); // #230: on the stopping task, for the app
        stopped               = hook();
        lichen_ctx_leave(previous);
    }
    if (!stopped) {
        ESP_LOGE(TAG, "%s is still running", name);
    }
    size_t tasks_left = lichen_os_task_count(ctx);
    lichen_sys_life_stop_end(ctx, stopped, (uint32_t)tasks_left);
    report(name, ctx, tasks_left);
    stop_timers(name, ctx);
    return stopped;
}

// Every app, from a snapshot of the contexts taken first: an app joining or leaving while the hooks run (seconds) cannot
// make the loop skip one or stop one twice, and an update's stop-all must stop every app that was there (#520)
bool lichen_sys_apps_stop(life_reason_t reason) {
    bool         all_stopped = true;
    size_t       n           = lichen_app_count();
    lichen_ctx_t *ctxs       = lichen_alloc(lichen_sys_ctx_os(), (n + 1) * sizeof(*ctxs), LICHEN_MEM_BULK);
    if (!ctxs) { // no memory: one by one, each ctx read as its turn comes
        for (size_t i = 0; i < lichen_app_count(); i++) {
            all_stopped = stop_one(lichen_sys_app_ctx_at(i), reason) && all_stopped;
        }
        return all_stopped;
    }
    for (size_t i = 0; i < n; i++) {
        ctxs[i] = lichen_sys_app_ctx_at(i);
    }
    for (size_t i = 0; i < n; i++) {
        all_stopped = stop_one(ctxs[i], reason) && all_stopped;
    }
    lichen_free(lichen_sys_ctx_os(), ctxs);
    return all_stopped;
}
