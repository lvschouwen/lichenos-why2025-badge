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

#include <stdatomic.h>
#include <stdlib.h>

static char const TAG[] = "ui";

#define UI_CALL_LOCK_MS 2000 // as the dialog: a render takes milliseconds, a lock held this long is a stuck LVGL task

static void run_as_owner(ui_call_fn_t fn, void *arg, uint32_t owner);
static bool fallback_call(ui_call_fn_t fn, void *arg);
static bool fallback_cancel(ui_call_fn_t fn, void *arg);
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

bool lichen_sys_ui_lock_held(void) {
    return !ui_lock_owner_armed(&s_owner) || ui_lock_owner_check(&s_owner, self_task()) == UI_LOCK_OK;
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

// The burst past the queue's size (#541): an app's call that did not fit is one LVGL async call of its own, which a FIFO
// barrier cannot order against, so it is booked here (LVGL lock held) with its owner and runs as it, and can be taken back by
// owner before the app's image is freed. The OS's own calls stay plain async calls; an app's call with no memory for its node is refused.
typedef struct fb_call {
    struct fb_call *next;
    ui_call_fn_t    fn;
    void           *arg;
    uint32_t        owner;
} fb_call_t;
static fb_call_t *s_fb;
static uint32_t   s_fb_owner; // the owner of the call being handed over: written by lichen_sys_ui_call, and fallback_call is
                                // only valid inside its hand_over, both under the LVGL lock

static void fb_unlink(fb_call_t *n) {
    for (fb_call_t **pp = &s_fb; *pp; pp = &(*pp)->next) {
        if (*pp == n) {
            *pp = n->next;
            return;
        }
    }
}

static void fb_run(void *node) { // LVGL task, lock held
    fb_call_t *n = node;
    fb_unlink(n);
    ui_call_fn_t fn    = n->fn;
    void        *arg   = n->arg;
    uint32_t     owner = n->owner;
    free(n);
    run_as_owner(fn, arg, owner);
}

static bool fallback_call(ui_call_fn_t fn, void *arg) {
    if (s_fb_owner == lichen_sys_ctx_os()) {
        return async_call(fn, arg);
    }
    fb_call_t *n = malloc(sizeof(*n));
    if (!n) {
        return false; // an app's call that cannot be booked cannot be taken back by owner either: refused, not run later (#541)
    }
    *n = (fb_call_t){s_fb, fn, arg, s_fb_owner};
    if (!async_call(fb_run, n)) {
        free(n);
        return false;
    }
    s_fb = n;
    return true;
}

// Every fallback call of the pair (fn, arg) (or, with fn NULL, of owner) is taken back; the plain one too (fn, arg)
static void fb_cancel_matching(ui_call_fn_t fn, void *arg, uint32_t owner) {
    for (fb_call_t *n = s_fb, *next; n; n = next) {
        next = n->next;
        if (fn ? (n->fn == fn && n->arg == arg) : n->owner == owner) {
            lv_async_call_cancel(fb_run, n);
            fb_unlink(n);
            free(n);
        }
    }
}

static bool fallback_cancel(ui_call_fn_t fn, void *arg) {
    fb_cancel_matching(fn, arg, 0);
    return async_cancel(fn, arg);
}

static atomic_uint s_refused, s_refused_wait_max; // any task writes: the max is a compare-and-swap

void lichen_sys_ui_refused(uint32_t *count, uint32_t *longest_ms) {
    *count      = atomic_load(&s_refused);
    *longest_ms = atomic_load(&s_refused_wait_max);
}

static void note_refused(uint32_t waited_ms) {
    atomic_fetch_add(&s_refused, 1);
    unsigned seen = atomic_load(&s_refused_wait_max);
    while (waited_ms > seen && !atomic_compare_exchange_weak(&s_refused_wait_max, &seen, waited_ms)) {
    }
}

bool lichen_sys_ui_call(void (*fn)(void *), void *arg, uint32_t timeout_ms) {
    if (!fn) {
        return false;
    }
    lichen_ctx_t owner = lichen_ctx_self();
    if (!lichen_sys_ui_lock(timeout_ms)) {
        note_refused(timeout_ms);
        ESP_LOGW(TAG, "ui call %p not queued: the LVGL lock was not free in %u ms", (void *)fn, (unsigned)timeout_ms);
        return false;
    }
    s_fb_owner = owner;
    bool ok    = ui_call_queue_hand_over(&s_queue, fn, arg, owner, async_call, fallback_call);
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
    ui_call_queue_cancel_all(&s_queue, fn, arg, fallback_cancel);
    lichen_sys_ui_unlock();
}

bool lichen_sys_ui_cancel_owner(uint32_t ctx, uint32_t wait_ms) {
    if (!lichen_sys_ui_lock(wait_ms)) {
        return !port_allows_lock(); // no display: nothing was queued
    }
    ui_call_queue_cancel_owner(&s_queue, ctx);
    fb_cancel_matching(NULL, NULL, ctx);
    lichen_sys_ui_unlock();
    return true;
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
