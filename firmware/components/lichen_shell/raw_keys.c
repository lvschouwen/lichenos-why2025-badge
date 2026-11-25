#include "raw_keys.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "key_cause.h"
#include "lichen_keys.h"
#include "lichen_sys_keys.h"
#include "lichen_app.h"
#include "lichen_sys_app.h"
#include "lichen_sys_ui.h"
#include "lvgl.h"
#include "raw_owner.h"
#include "shell_parts.h"
#include "shell_ui_ops.h"
#include "key_matrix.h"
#include "video_notice.h"

#include <stdatomic.h>

static char const TAG[] = "raw_keys";

#define MAX_LISTENERS 4
#define QUEUE_DEPTH   64
#define DRAIN_MS      20

static struct {
    raw_keys_cb cb;
    void       *arg;
    bool        swallow;
} listeners[MAX_LISTENERS];
static QueueHandle_t queue;
static lv_timer_t   *timer;
static bool          hooked;
static atomic_int    active;  // listeners registered: the hook queues nothing while 0
static atomic_int    swallowing;
static atomic_bool   test_open; // the Keyboard test holds the keys
static atomic_bool   held;      // any app holds them: the shape-key shortcuts stay quiet
static atomic_bool   esc_down;    // keyboard task writes, the escape sampler reads (#563)
static atomic_uint   esc_presses; // counted, so a release and press between two samples is seen

void raw_keys_set_test_open(bool open) {
    atomic_store(&test_open, open);
}

bool raw_keys_test_open(void) {
    return atomic_load(&test_open);
}

bool raw_keys_held(void) {
    return atomic_load(&held);
}

// Keyboard task
static bool hook(key_raw_event_t const *ev, void *arg) {
    (void)arg;
    if (ev->code == KEY_MATRIX_ESC_CODE) {
        atomic_store(&esc_down, ev->pressed);
        if (ev->pressed) {
            atomic_fetch_add(&esc_presses, 1);
        }
    }
    if (atomic_load(&active) > 0) {
        key_raw_event_t stamped = *ev;
        raw_owner_stamp(&stamped, atomic_load(&test_open), atomic_load(&held));
        xQueueSend(queue, &stamped, 0); // a full queue drops events rather than blocking the keyboard
    }
    return atomic_load(&swallowing) > 0;
}

static void drain(lv_timer_t *t) {
    (void)t;
    key_raw_event_t ev;
    while (xQueueReceive(queue, &ev, 0) == pdTRUE) {
        // What a listener does is this key's (#400): a shape-key shortcut that opens MeshCore starts it only for a hand
        key_cause_t previous = key_cause_enter(ev.injected ? KEY_CAUSE_INJECTED : KEY_CAUSE_HAND);
        for (int i = 0; i < MAX_LISTENERS; i++) {
            if (listeners[i].cb) {
                listeners[i].cb(&ev, listeners[i].arg);
            }
        }
        key_cause_leave(previous);
    }
}

static bool ensure_started(void) {
    if (!queue) {
        queue = xQueueCreate(QUEUE_DEPTH, sizeof(key_raw_event_t));
    }
    if (queue && !hooked) {
        hooked = lichen_sys_keys_hook_add(hook, NULL);
    }
    if (queue && hooked && !timer) {
        timer = lv_timer_create(drain, DRAIN_MS, NULL);
    }
    return queue && hooked && timer;
}

bool raw_keys_listen(raw_keys_cb cb, void *arg, bool swallow) {
    if (!ensure_started()) {
        ESP_LOGE(TAG, "raw key hook not started");
        return false;
    }
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (!listeners[i].cb) {
            listeners[i].cb      = cb;
            listeners[i].arg     = arg;
            listeners[i].swallow = swallow;
            xQueueReset(queue); // nothing from before the listener existed
            lv_timer_resume(timer);
            atomic_fetch_add(&active, 1);
            if (swallow) {
                atomic_fetch_add(&swallowing, 1);
            }
            return true;
        }
    }
    ESP_LOGE(TAG, "no room for another raw key listener");
    return false;
}

void raw_keys_unlisten(raw_keys_cb cb, void *arg) {
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (listeners[i].cb == cb && listeners[i].arg == arg) {
            if (listeners[i].swallow) {
                atomic_fetch_sub(&swallowing, 1);
            }
            atomic_fetch_sub(&active, 1);
            listeners[i].cb = NULL;
            if (atomic_load(&active) == 0) {
                lv_timer_pause(timer);
            }
        }
    }
}

// lichen_keys.h (#180): the app's side of the raw keys, in the API's own event type (key_raw.h is board's). One app at a
// time. Only the Keyboard test marks the screen as such (#534): taking the keys stamps its events as captured inside it.
// A game just owns the keys (held), which keeps the shortcuts and remote keys off it.
static lichen_raw_key_fn app_fn;
static void             *app_arg;
static raw_owner_t       owner; // under the LVGL lock, like the listener table

static void to_app(key_raw_event_t const *ev, void *arg) {
    (void)arg;
    if (app_fn) {
        // A held key sends no repeats and a release is no wake press: the owner's keys are activity (the saver, dim)
        screen_idle_key();
        lichen_raw_key_t key = {.code = ev->code, .pressed = ev->pressed, .shift = ev->shift};
        app_fn(&key, app_arg);
    }
}

static bool caller_is_keytest(lichen_ctx_t ctx) {
    lichen_ctx_t keytest = LICHEN_CTX_NONE;
    return lichen_app_by_name("keytest", &keytest) && raw_owner_is_keytest(ctx, keytest);
}

bool lichen_keys_raw_take_impl(lichen_raw_key_fn fn, void *arg) {
    lichen_ctx_t ctx = lichen_ctx_self();
    if (!fn || app_fn) {
        ESP_LOGW(TAG, "raw keys %s", fn ? "already taken by an app" : "taken without a listener");
        return false;
    }
    if (!raw_owner_take(&owner, ctx, caller_is_keytest(ctx))) {
        ESP_LOGW(TAG, "raw keys: no app context");
        return false;
    }
    atomic_store(&test_open, raw_owner_test_mark(&owner)); // before the first event can be queued
    atomic_store(&held, true);
    if (!raw_keys_listen(to_app, NULL, true)) {
        raw_owner_release_ctx(&owner, ctx);
        atomic_store(&test_open, false);
        atomic_store(&held, false);
        return false;
    }
    app_fn  = fn;
    app_arg = arg;
    return true;
}

static void free_keys(lichen_ctx_t ctx) {
    if (!raw_owner_release_ctx(&owner, ctx)) {
        return;
    }
    raw_keys_unlisten(to_app, NULL);
    app_fn  = NULL;
    app_arg = NULL;
    atomic_store(&test_open, false);
    atomic_store(&held, false);
}

void lichen_keys_raw_release_impl(lichen_raw_key_fn fn, void *arg) {
    if (!app_fn || app_fn != fn || app_arg != arg) {
        return;
    }
    free_keys(owner.ctx); // fn and arg are the proof of ownership
}

// Any task (the SD app loader, #533): the table and the drain timer are LVGL's, so take the (recursive) lock
bool raw_keys_release_ctx(lichen_ctx_t ctx) {
    if (ctx == LICHEN_CTX_NONE) {
        return true;
    }
    if (!lichen_sys_ui_lock(0)) {
        ESP_LOGW(TAG, "raw keys of ctx %u not released: no LVGL lock", (unsigned)ctx);
        return false;
    }
    free_keys(ctx);
    bool free_of_ctx = owner.ctx != ctx;
    lichen_sys_ui_unlock();
    return free_of_ctx;
}

// The escape sampler (#563): LVGL task, every ESC_SAMPLE_MS. It reads only the keyboard task's two atomics, never an app's
// state or the listener queue, so what an app does with its raw keys cannot hold it back. Only an app that hangs the LVGL
// task itself, or holds the UI lock, can: then nothing here runs.
#define ESC_SAMPLE_MS 100

static void leave_video_app(void) {
    if (video_notice_raise(VN_ESCAPE)) {
        free_keys(owner.ctx); // the hung app's raw hold must not outlive the screen: the keyboard would stay swallowed
        shell_back_impl();    // the same leave as a short Esc on the app's screen
    }
}

static void escape_tick(lv_timer_t *t) {
    (void)t;
    static video_escape_t esc;
    if (video_escape_sample(&esc, video_notice_active(), atomic_load(&esc_down), atomic_load(&esc_presses), lv_tick_get())) {
        leave_video_app();
    }
}

void raw_keys_start_escape(void) {
    if (!ensure_started()) { // the hook is shared with the listeners; its timer stays paused until one registers
        ESP_LOGE(TAG, "escape hook not started");
        return;
    }
    if (atomic_load(&active) == 0) {
        lv_timer_pause(timer);
    }
    lv_timer_create(escape_tick, ESC_SAMPLE_MS, NULL);
}
