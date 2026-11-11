// The UI service (#225): the only code that takes the LVGL port lock (board's port setup aside) and sets LVGL's default
// group. The shell and main take the lock through lichen_sys_ui.h, apps hand work over with lichen_ui_call
// (lichen_ui.h); #221 puts the app calls behind the gate's ops table. Moved out of lichen_api by #220.
//
// Calls for the LVGL task wait in a small FIFO (ui_call_queue.c) and one LVGL async call drains each batch: the same
// scheduling as one lv_async_call per call had (the drain runs on the next lv_timer_handler pass, in the same pass when
// queued from the LVGL task) and one allocation per batch instead of per call. FIFO: since #286 no code outside this
// file calls lv_async_call (tools/layers.conf), so every deferred call in the OS runs in the order it was queued,
// unless a burst overflows the queue.
#include "lichen_os.h"
#include "lichen_sys_app.h"
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

static void run_as_owner(ui_call_fn_t fn, void *arg, uint32_t owner);
static ui_call_queue_t s_queue = {.run = run_as_owner}; // LVGL lock held
static ui_lock_owner_t s_owner;     // also read by the lock-owner check (ui_lock_check.c)

static uintptr_t self_task(void) {
    return (uintptr_t)xTaskGetCurrentTaskHandle();
}

void lichen_ui_service_port_up(void) {
    ui_lock_owner_set_port_up(&s_owner);
}

// With no display lvgl_port_lock would assert (#248): refuse, and say so once.
static bool port_allows_lock(void) {
    bool first;
    if (ui_lock_owner_may_lock(&s_owner, &first)) {
        return true;
    }
    if (first) {
        ESP_LOGW(TAG, "no display: UI calls are refused");
    }
    return false;
}

bool lichen_sys_ui_lock(uint32_t timeout_ms) {
    if (!port_allows_lock()) {
        return false;
    }
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

// The fallback's counterpart: one that a full queue handed to LVGL directly (ui_call_queue_hand_over)
static bool async_cancel(ui_call_fn_t fn, void *arg) {
    return lv_async_call_cancel(fn, arg) == LV_RESULT_OK;
}

// A queued call runs for the app that queued it (#230): its hooks see lichen_ctx_self() as they did when they queued
// it. TLS only, no lock: the drain already holds the LVGL lock and this takes nothing more.
static void run_as_owner(ui_call_fn_t fn, void *arg, uint32_t owner) {
    lichen_ctx_t previous = lichen_ctx_enter(owner);
    fn(arg);
    lichen_ctx_leave(previous);
}

bool lichen_sys_ui_call(void (*fn)(void *), void *arg, uint32_t timeout_ms) {
    if (!fn) {
        return false;
    }
    lichen_ctx_t owner = lichen_ctx_self();
    if (!lichen_sys_ui_lock(timeout_ms)) {
        ESP_LOGW(TAG, "ui call %p not queued: the LVGL lock was not free in %u ms", (void *)fn, (unsigned)timeout_ms);
        return false;
    }
    bool ok = ui_call_queue_hand_over(&s_queue, fn, arg, owner, async_call, async_call);
    lichen_sys_ui_unlock();
    if (!ok) {
        ESP_LOGE(TAG, "ui call %p not queued: out of memory", (void *)fn);
    }
    return ok;
}

void lichen_sys_ui_cancel(void (*fn)(void *), void *arg) {
    if (!fn || !lichen_sys_ui_lock(0)) {
        return; // no display: nothing was queued
    }
    ui_call_queue_cancel_all(&s_queue, fn, arg, async_cancel);
    lichen_sys_ui_unlock();
}

bool lichen_ui_call(void (*fn)(void *), void *arg) {
    return lichen_sys_ui_call(fn, arg, UI_CALL_LOCK_MS);
}

void lichen_ui_with_group(lv_group_t *group, void (*build)(void *), void *arg) {
    if (!ui_lock_owner_port_is_up(&s_owner)) {
        return; // no display: nothing to build on
    }
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
