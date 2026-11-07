// The UI service (#225): the only code that takes the LVGL port lock (board's port setup aside) and sets LVGL's default
// group. The shell and main take the lock through lichen_sys_ui.h, apps hand work over with lichen_ui_call
// (lichen_ui.h); #221 puts the app calls behind the gate's ops table. Moved out of lichen_api by #220.
//
// Calls for the LVGL task wait in a small FIFO (ui_call_queue.c) and one LVGL async call drains each batch: the same
// scheduling as one lv_async_call per call had (the drain runs on the next lv_timer_handler pass, in the same pass when
// queued from the LVGL task) and one allocation per batch instead of per call. FIFO among UI-service calls; a raw
// lv_async_call (LVGL-task code still makes them) stays newest first, and the two do not order against each other.
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
static ui_lock_owner_t s_owner;     // also read by the lock-owner check (ui_lock_check.c)

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
    if (!ui_lock_owner_armed(&s_owner)) {
        return UI_LOCK_UNARMED; // boot: one load, no task lookup
    }
    return ui_lock_owner_check(&s_owner, self_task());
}

bool ui_service_armed(void) {
    return ui_lock_owner_armed(&s_owner);
}

// lv_async_call creates an LVGL timer: only under the lock, or it races the LVGL task's timer list (#118). It is both
// the drain's scheduler and the full queue's fallback (ui_call_queue_hand_over).
static bool async_call(ui_call_fn_t fn, void *arg) {
    return lv_async_call(fn, arg) == LV_RESULT_OK;
}

bool lichen_sys_ui_call(void (*fn)(void *), void *arg, uint32_t timeout_ms) {
    if (!fn) {
        return false;
    }
    if (!lichen_sys_ui_lock(timeout_ms)) {
        ESP_LOGW(TAG, "ui call %p not queued: the LVGL lock was not free in %u ms", (void *)fn, (unsigned)timeout_ms);
        return false;
    }
    bool ok = ui_call_queue_hand_over(&s_queue, fn, arg, async_call, async_call);
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

#if CONFIG_LICHEN_UI_LOCK_CHECK
static void arm(void *unused) {
    (void)unused;
    ui_lock_owner_arm(&s_owner, self_task()); // runs on the LVGL task: that is its handle
}
#endif

void lichen_ui_service_arm(void) {
#if CONFIG_LICHEN_UI_LOCK_CHECK
    if (!lichen_sys_ui_call(arm, NULL, 0)) {
        ESP_LOGW(TAG, "lock-owner check not armed");
    }
#endif
}
