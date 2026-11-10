// The Timer app's service (#152): the countdown and stopwatch state machines of timer_calc.c on esp_timer's clock, and
// the poll that tells the user when the countdown ends. The notice goes through the OS: lichen_notify (a toast over
// whatever is on screen, the status bar dot, the notification centre) and lichen_badge_set (the tile). A sound comes
// with #67 and a buzz with #68.
#include "timer_service.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lichen_badge.h"
#include "lichen_notify.h"
#include "lichen_ui.h"
#include "lvgl.h"

#include <stdio.h>

static char const TAG[]    = "timer";
static char const APP_ID[] = "timer";

#define POLL_MS 250 // the end is noticed within a quarter of a second, whatever screen is up

static tcd_t s_cd;
EXT_RAM_BSS_ATTR static sw_t s_sw;
static bool  s_inited;

static void ensure_init(void) {
    if (!s_inited) {
        tcd_init(&s_cd);
        tcd_set(&s_cd, 5 * 60 * 1000); // 5 minutes until the user sets another
        sw_init(&s_sw);
        s_inited = true;
    }
}

tcd_t *timer_svc_countdown(void) {
    ensure_init();
    return &s_cd;
}

sw_t *timer_svc_stopwatch(void) {
    ensure_init();
    return &s_sw;
}

int64_t timer_svc_now(void) {
    return esp_timer_get_time() / 1000;
}

void timer_svc_ack(void) {
    lichen_badge_set(APP_ID, 0);
    lichen_notify_mark_read(APP_ID, NULL);
}

static void poll(lv_timer_t *timer) {
    (void)timer;
    ensure_init();
    if (tcd_poll(&s_cd, timer_svc_now())) {
        char words[32], body[64];
        tfmt_words(words, sizeof(words), s_cd.set_ms);
        snprintf(body, sizeof(body), "The %s timer is up", words);
        ESP_LOGI(TAG, "countdown of %s finished", words);
        lichen_notify(APP_ID, LN_WARN, "Timer finished", body, 0);
        lichen_badge_set(APP_ID, 1);
    }
}

static void start_poll(void *arg) {
    (void)arg;
    if (!lv_timer_create(poll, POLL_MS, NULL)) {
        ESP_LOGE(TAG, "no poll timer: a finished countdown will not be noticed");
    }
}

// timer_app_register runs on the main task (start_apps), outside the LVGL task: the poll timer is made there (#242)
void timer_svc_start(void) {
    ensure_init();
    if (!lichen_ui_call(start_poll, NULL)) {
        ESP_LOGE(TAG, "poll timer not queued: a finished countdown will not be noticed");
    }
}
