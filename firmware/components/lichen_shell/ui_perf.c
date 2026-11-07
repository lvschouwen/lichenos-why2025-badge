// "fw ui perf" (#93): frame timing of the display, from LVGL's display events, so the cost of a screen (and of the
// PSRAM clock) is a number instead of a feeling. "fw ui perf bench [n]" redraws the whole current screen n times under
// the LVGL lock and reports the time per frame: the same screen, the same number, whatever else is going on. The Bench
// app (#96) uses the same two calls (ui_perf.h).
#include "ui_perf.h"

#include "lichen_sys_ui.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "lvgl.h"
#include "shell_parts.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_MAX 50

static ui_perf_t     s_perf;
static ui_perf_lat_t s_lat;
static int64_t   s_render_t0, s_flush_t0, s_wait_t0;
// Key to flush (#205): the press time of the oldest key not yet shown, and whether a render started after it (a flush
// that was already running when the key came in does not show it). 0 = no key waiting.
static int64_t s_key_t0;
static bool    s_key_rendered;
// Boot to first frame (#205): the first flush ever, in ms since boot; not cleared by a reset (it is a boot fact)
static uint32_t s_first_frame_ms;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void on_flush_finish(int64_t now) {
    if (s_first_frame_ms == 0) {
        s_first_frame_ms = (uint32_t)(now / 1000) ? (uint32_t)(now / 1000) : 1;
    }
    if (s_key_t0 && s_key_rendered) {
        perf_acc_add(&s_lat.key, (uint32_t)(now - s_key_t0));
        perf_hist_add(&s_lat.key_hist, (uint32_t)(now - s_key_t0));
        s_key_t0       = 0;
        s_key_rendered = false;
    }
}

static void on_key_press(lv_event_t *e) {
    (void)e;
    if (!s_key_t0) {
        s_key_t0 = esp_timer_get_time();
    }
}

static void on_display(lv_event_t *e) {
    int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(e)) {
        case LV_EVENT_RENDER_START:
            s_render_t0    = now;
            s_key_rendered = s_key_rendered || s_key_t0 != 0;
            break;
        case LV_EVENT_RENDER_READY:
            perf_acc_add(&s_perf.render, (uint32_t)(now - s_render_t0));
            perf_hist_add(&s_lat.render_hist, (uint32_t)(now - s_render_t0));
            break;
        case LV_EVENT_FLUSH_START: s_flush_t0 = now; break;
        case LV_EVENT_FLUSH_FINISH:
            perf_acc_add(&s_perf.flush, (uint32_t)(now - s_flush_t0));
            on_flush_finish(now);
            break;
        case LV_EVENT_FLUSH_WAIT_START: s_wait_t0 = now; break;
        case LV_EVENT_FLUSH_WAIT_FINISH: perf_acc_add(&s_perf.wait, (uint32_t)(now - s_wait_t0)); break;
        default: break;
    }
}

void ui_perf_snapshot(ui_perf_t *out) {
    *out = s_perf;
}

uint64_t ui_perf_redraw_us(unsigned n) {
    lv_display_t *disp = lv_display_get_default();
    int64_t       t0   = esp_timer_get_time();
    for (unsigned i = 0; i < n; i++) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(disp);
    }
    return (uint64_t)(esp_timer_get_time() - t0);
}

static void bench(unsigned n) {
    n = n == 0 ? 10 : n > BENCH_MAX ? BENCH_MAX : n;
    lichen_sys_ui_lock(0);
    uint32_t renders = s_perf.render.n;
    uint64_t us      = ui_perf_redraw_us(n);
    renders          = s_perf.render.n - renders;
    lichen_sys_ui_unlock();
    unsigned per10 = (unsigned)((us / n + 50) / 100);
    console_reply("ui perf bench: %u full-screen redraws in %u ms, %u.%u ms per frame (%u renders counted)", n,
                  (unsigned)(us / 1000), per10 / 10, per10 % 10, (unsigned)renders);
}

void ui_perf_cmd(char const *args) {
    while (*args == ' ') {
        args++;
    }
    if (strncmp(args, "reset", 5) == 0) {
        lichen_sys_ui_lock(0);
        ui_perf_reset(&s_perf, now_ms());
        s_lat = (ui_perf_lat_t){0};
        lichen_sys_ui_unlock();
        console_reply("ui perf: counters reset");
        return;
    }
    if (strncmp(args, "bench", 5) == 0) {
        bench((unsigned)atoi(args + 5));
        return;
    }
    // Formatted under the lock (no copy of the histograms onto the console task's small stack), replied after it
    char line[200], line2[200];
    lichen_sys_ui_lock(0);
    ui_perf_format(line, sizeof(line), &s_perf, now_ms());
    ui_perf_format_latency(line2, sizeof(line2), &s_lat, s_first_frame_ms);
    lichen_sys_ui_unlock();
    console_reply("%s", line);
    console_reply("%s", line2);
}

void ui_perf_early(lv_display_t *disp) {
    static lv_event_code_t const codes[] = {LV_EVENT_RENDER_START,     LV_EVENT_RENDER_READY,    LV_EVENT_FLUSH_START,
                                            LV_EVENT_FLUSH_FINISH,     LV_EVENT_FLUSH_WAIT_START, LV_EVENT_FLUSH_WAIT_FINISH};
    if (!disp) {
        return;
    }
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        lv_display_add_event_cb(disp, on_display, codes[i], NULL);
    }
}

void ui_perf_watch_keys(lv_indev_t *keys) {
    lv_indev_add_event_cb(keys, on_key_press, LV_EVENT_PRESSED, NULL); // the keypad comes after shell_start
}

void ui_perf_start(void) {
    s_key_t0 = 0;
    s_lat    = (ui_perf_lat_t){0};
    ui_perf_reset(&s_perf, now_ms());
}
