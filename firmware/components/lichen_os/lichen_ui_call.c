// The UI service (#225): the only code that takes the LVGL port lock (board's port setup aside) and sets LVGL's default
// group. The shell and main take the lock through lichen_sys_ui.h, apps hand work over with lichen_ui_call
// (lichen_ui.h); #221 puts the app calls behind the gate's ops table. Moved out of lichen_api by #220.
//
// Calls for the LVGL task wait in a small FIFO (ui_call_queue.c) and one LVGL async call drains each batch: the same
// scheduling as one lv_async_call per call had (the drain runs on the next lv_timer_handler pass, in the same pass when
// queued from the LVGL task), one allocation per batch instead of per call, and calls run in the order they came.
#include "lichen_os.h"
#include "lichen_sys_ui.h"
#include "lichen_ui.h"
#include "ui_call_queue.h"
#include "ui_service.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static char const TAG[] = "ui";

#define UI_CALL_LOCK_MS 2000 // as the dialog: a render takes milliseconds, a lock held this long is a stuck LVGL task

static ui_call_queue_t s_queue;     // LVGL lock held
static ui_lock_owner_t s_owner;     // also read by the lock-owner check (ui_lock_assert.c)

static uintptr_t self_task(void) {
    return (uintptr_t)xTaskGetCurrentTaskHandle();
}

bool lichen_sys_ui_lock(uint32_t timeout_ms) {
    if (!lvgl_port_lock(timeout_ms)) {
        return false;
    }
    ui_lock_owner_taken(&s_owner, self_task());
    return true;
}

void lichen_sys_ui_unlock(void) {
    if (!ui_lock_owner_given(&s_owner, self_task())) {
        ESP_LOGE(TAG, "unlock by %s without its lichen_sys_ui_lock", pcTaskGetName(NULL));
        assert(!"lichen_sys_ui_unlock without the lock");
        return; // NDEBUG: never give a mutex this task does not hold
    }
    lvgl_port_unlock();
}

ui_lock_verdict_t ui_service_lock_check(void) {
    return ui_lock_owner_check(&s_owner, self_task());
}

// LVGL task, under the lock: the batch queued before this drain started. A call that queues another one schedules the
// next drain (ui_call_queue_begin_drain), so it never runs inline here.
static void drain(void *unused) {
    (void)unused;
    size_t    n = ui_call_queue_begin_drain(&s_queue);
    ui_call_t call;
    while (n-- > 0 && ui_call_queue_pop(&s_queue, &call)) {
        call.fn(call.arg);
    }
}

// Lock held: queued, or (a burst past the queue) an async call of its own as before #225
static bool hand_over(void (*fn)(void *), void *arg) {
    switch (ui_call_queue_push(&s_queue, fn, arg)) {
    case UI_CALL_QUEUED:
        return true;
    case UI_CALL_QUEUED_SCHEDULE:
        // lv_async_call creates an LVGL timer: only under the lock, or it races the LVGL task's timer list (#118)
        if (lv_async_call(drain, NULL) == LV_RESULT_OK) {
            return true;
        }
        ui_call_queue_unpush(&s_queue);
        return false;
    case UI_CALL_FULL:
    default:
        return lv_async_call(fn, arg) == LV_RESULT_OK;
    }
}

bool lichen_sys_ui_call(void (*fn)(void *), void *arg, uint32_t timeout_ms) {
    if (!fn) {
        return false;
    }
    if (!lichen_sys_ui_lock(timeout_ms)) {
        ESP_LOGW(TAG, "ui call %p not queued: the LVGL lock was not free in %u ms", (void *)fn, (unsigned)timeout_ms);
        return false;
    }
    bool ok = hand_over(fn, arg);
    lichen_sys_ui_unlock();
    if (!ok) {
        ESP_LOGE(TAG, "ui call %p not queued: out of memory", (void *)fn);
    }
    return ok;
}

bool lichen_ui_call(void (*fn)(void *), void *arg) {
    return lichen_sys_ui_call(fn, arg, UI_CALL_LOCK_MS);
}

void lichen_ui_with_group(lv_group_t *group, void (*build)(void *), void *arg) {
    lv_group_t *previous = lv_group_get_default();
    lv_group_set_default(group);
    build(arg);
    lv_group_set_default(previous); // in one place, so it can never be left set to a screen that is gone
}

#if CONFIG_LICHEN_UI_LOCK_ASSERT
static void arm(void *unused) {
    (void)unused;
    ui_lock_owner_arm(&s_owner, self_task()); // runs on the LVGL task: that is its handle
}
#endif

void lichen_ui_service_arm(void) {
#if CONFIG_LICHEN_UI_LOCK_ASSERT
    if (!lichen_sys_ui_call(arm, NULL, 0)) {
        ESP_LOGW(TAG, "lock-owner check not armed");
    }
#endif
}
