// The clock saved every minute (#326). A reset that skips IDF's time write-back, or one that loses the clock while the
// power stays on, otherwise brings back a clock stale by however long the badge ran. The record is 12 bytes of LP RAM
// (RTC_NOINIT_ATTR): no flash wear and no flash write stalling the display (#31), written from the esp_timer task
// (no ISR) and after every accepted set. It survives what keeps the LP RAM powered (core resets: esp_restart, panic,
// the watchdogs; whether a chip reset by EN keeps it is the bench's to show) and never a power-off (no VBAT): a save
// can then not pass a stale time off as carried. #4 doubted RTC no-init memory over a software restart (BadgeVMS's
// layout; a later panic reset showed it kept); IDF itself keeps its RTC timer state there (esp_clk.c) for the same
// resets. The decision is clock_boot_pick() (clock_keep.c, host-tested).
#include "clock_keep.h"
#include "lichen_clock.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include <time.h>

static RTC_NOINIT_ATTR volatile clock_rec_t s_rec;
static portMUX_TYPE                         s_rec_mux = portMUX_INITIALIZER_UNLOCKED; // timer task vs proposers
static clock_boot_info_t                    s_boot;
static int64_t                              s_saved_at_s = -1; // RTC seconds of the newest save (under s_rec_mux)
static bool                                 s_armed;           // the boot choice ran: saving cannot hide a record

static int64_t rtc_now_s(void) {
    return (int64_t)(esp_clk_rtc_time() / 1000000);
}

static clock_rec_t rec_read(void) {
    taskENTER_CRITICAL(&s_rec_mux);
    clock_rec_t r = {.wall_s = s_rec.wall_s, .rtc_s = s_rec.rtc_s, .check = s_rec.check};
    taskEXIT_CRITICAL(&s_rec_mux);
    return r;
}

// Never a clock that is not set: a save of the build-epoch fallback would outrank a real carried time
static void save_now(void) {
    if (!s_armed || !clock_is_set()) {
        return;
    }
    int64_t     rtc = rtc_now_s();
    clock_rec_t r   = clock_rec_make((uint32_t)time(NULL), (uint32_t)rtc);
    taskENTER_CRITICAL(&s_rec_mux);
    s_rec.check  = 0; // a reset between these stores leaves an invalid record, never a mixed one
    s_rec.wall_s = r.wall_s;
    s_rec.rtc_s  = r.rtc_s;
    s_rec.check  = r.check;
    s_saved_at_s = rtc;
    taskEXIT_CRITICAL(&s_rec_mux);
}

static void on_save_timer(void *arg) {
    (void)arg;
    save_now();
}

static void choose_at_boot(void) {
    clock_rec_t r   = rec_read();
    int64_t     rtc = rtc_now_s();
    int64_t     t   = 0;
    s_boot.carried_s  = (int64_t)time(NULL);
    s_boot.at_ms      = esp_timer_get_time() / 1000;
    s_boot.rec_valid  = clock_rec_valid(&r);
    s_boot.saved_s    = s_boot.rec_valid ? clock_rec_estimate(&r, rtc) : 0;
    s_boot.save_age_s = s_boot.rec_valid && rtc >= (int64_t)r.rtc_s ? rtc - (int64_t)r.rtc_s : -1;
    s_boot.pick       = clock_boot_pick(s_boot.carried_s, &r, rtc, clock_build_epoch(), &t);
    if (s_boot.pick == CLOCK_BOOT_SAVED && !clock_restore_saved(t)) {
        ESP_LOGW("clock", "saved clock %lld not applied", (long long)t);
        s_boot.pick = clock_is_set() ? CLOCK_BOOT_CARRIED : CLOCK_BOOT_NONE;
    }
    ESP_LOGI("clock", "boot: carried %lld, saved %lld, using %s", (long long)s_boot.carried_s,
             (long long)s_boot.saved_s, clock_boot_text(s_boot.pick));
}

void clock_boot_init(void) {
    static bool done; // the boot task calls this, early in app_main and again from clock_console_register
    if (done) {
        return;
    }
    done = true;
    choose_at_boot();
    s_armed = true;
    clock_set_saver(save_now);
    save_now();
    esp_timer_create_args_t const args = {.callback              = on_save_timer,
                                          .dispatch_method       = ESP_TIMER_TASK,
                                          .name                  = "clock_save",
                                          .skip_unhandled_events = true};
    esp_timer_handle_t timer = NULL;
    if (esp_timer_create(&args, &timer) != ESP_OK ||
        esp_timer_start_periodic(timer, (uint64_t)CLOCK_SAVE_EVERY_S * 1000000) != ESP_OK) {
        ESP_LOGW("clock", "clock save timer not started: saved only when a source sets the clock");
    }
}

clock_boot_info_t clock_boot_info(void) {
    return s_boot;
}

int64_t clock_save_age_s(void) {
    taskENTER_CRITICAL(&s_rec_mux);
    int64_t at = s_saved_at_s;
    taskEXIT_CRITICAL(&s_rec_mux);
    return at < 0 ? -1 : rtc_now_s() - at;
}
