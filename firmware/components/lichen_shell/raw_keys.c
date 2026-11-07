#include "raw_keys.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lichen_keys.h"
#include "lichen_sys_keys.h"
#include "lvgl.h"
#include "shell_ui_ops.h"

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
static atomic_bool   test_open;

void raw_keys_set_test_open(bool open) {
    atomic_store(&test_open, open);
}

bool raw_keys_test_open(void) {
    return atomic_load(&test_open);
}

// Keyboard task
static bool hook(key_raw_event_t const *ev, void *arg) {
    (void)arg;
    if (atomic_load(&active) > 0) {
        key_raw_event_t stamped = *ev;
        stamped.test_open       = atomic_load(&test_open);
        xQueueSend(queue, &stamped, 0); // a full queue drops events rather than blocking the keyboard
    }
    return atomic_load(&swallowing) > 0;
}

static void drain(lv_timer_t *t) {
    (void)t;
    key_raw_event_t ev;
    while (xQueueReceive(queue, &ev, 0) == pdTRUE) {
        for (int i = 0; i < MAX_LISTENERS; i++) {
            if (listeners[i].cb) {
                listeners[i].cb(&ev, listeners[i].arg);
            }
        }
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
// time: it is the Keyboard test, so taking the keys also stamps the events as captured inside it.
static lichen_raw_key_fn app_fn;
static void             *app_arg;

static void to_app(key_raw_event_t const *ev, void *arg) {
    (void)arg;
    if (app_fn) {
        lichen_raw_key_t key = {.code = ev->code, .pressed = ev->pressed, .shift = ev->shift};
        app_fn(&key, app_arg);
    }
}

bool lichen_keys_raw_take_impl(lichen_raw_key_fn fn, void *arg) {
    if (!fn || app_fn) {
        ESP_LOGW(TAG, "raw keys %s", fn ? "already taken by an app" : "taken without a listener");
        return false;
    }
    raw_keys_set_test_open(true); // before the first event can be queued
    if (!raw_keys_listen(to_app, NULL, true)) {
        raw_keys_set_test_open(false);
        return false;
    }
    app_fn  = fn;
    app_arg = arg;
    return true;
}

void lichen_keys_raw_release_impl(lichen_raw_key_fn fn, void *arg) {
    if (!app_fn || app_fn != fn || app_arg != arg) {
        return;
    }
    raw_keys_unlisten(to_app, NULL);
    app_fn = NULL;
    raw_keys_set_test_open(false);
}
