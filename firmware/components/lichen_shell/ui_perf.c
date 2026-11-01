// "fw ui perf" (#93): frame timing of the display, from LVGL's display events, so the cost of a screen (and of the
// PSRAM clock) is a number instead of a feeling. "fw ui perf bench [n]" redraws the whole current screen n times under
// the LVGL lock and reports the time per frame: the same screen, the same number, whatever else is going on.
#include "ui_perf_stats.h"

#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "lvgl.h"
#include "shell_parts.h"

#include <stdlib.h>
#include <string.h>

#define BENCH_MAX 50

static ui_perf_t s_perf;
static int64_t   s_render_t0, s_flush_t0, s_wait_t0;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void on_display(lv_event_t *e) {
    int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(e)) {
        case LV_EVENT_RENDER_START: s_render_t0 = now; break;
        case LV_EVENT_RENDER_READY: perf_acc_add(&s_perf.render, (uint32_t)(now - s_render_t0)); break;
        case LV_EVENT_FLUSH_START: s_flush_t0 = now; break;
        case LV_EVENT_FLUSH_FINISH: perf_acc_add(&s_perf.flush, (uint32_t)(now - s_flush_t0)); break;
        case LV_EVENT_FLUSH_WAIT_START: s_wait_t0 = now; break;
        case LV_EVENT_FLUSH_WAIT_FINISH: perf_acc_add(&s_perf.wait, (uint32_t)(now - s_wait_t0)); break;
        default: break;
    }
}

static void bench(unsigned n) {
    n = n == 0 ? 10 : n > BENCH_MAX ? BENCH_MAX : n;
    lvgl_port_lock(0);
    lv_display_t *disp    = lv_display_get_default();
    uint32_t      renders = s_perf.render.n;
    int64_t       t0      = esp_timer_get_time();
    for (unsigned i = 0; i < n; i++) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(disp);
    }
    int64_t us = esp_timer_get_time() - t0;
    renders    = s_perf.render.n - renders;
    lvgl_port_unlock();
    unsigned per10 = (unsigned)((us / n + 50) / 100);
    console_reply("ui perf bench: %u full-screen redraws in %u ms, %u.%u ms per frame (%u renders counted)", n,
                  (unsigned)(us / 1000), per10 / 10, per10 % 10, (unsigned)renders);
}

void ui_perf_cmd(char const *args) {
    while (*args == ' ') {
        args++;
    }
    if (strncmp(args, "reset", 5) == 0) {
        lvgl_port_lock(0);
        ui_perf_reset(&s_perf, now_ms());
        lvgl_port_unlock();
        console_reply("ui perf: counters reset");
        return;
    }
    if (strncmp(args, "bench", 5) == 0) {
        bench((unsigned)atoi(args + 5));
        return;
    }
    char line[200];
    lvgl_port_lock(0);
    ui_perf_t copy = s_perf;
    lvgl_port_unlock();
    ui_perf_format(line, sizeof(line), &copy, now_ms());
    console_reply("%s", line);
}

void ui_perf_start(void) {
    static lv_event_code_t const codes[] = {LV_EVENT_RENDER_START,     LV_EVENT_RENDER_READY,    LV_EVENT_FLUSH_START,
                                            LV_EVENT_FLUSH_FINISH,     LV_EVENT_FLUSH_WAIT_START, LV_EVENT_FLUSH_WAIT_FINISH};
    lv_display_t *disp = lv_display_get_default();
    if (!disp) {
        return;
    }
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        lv_display_add_event_cb(disp, on_display, codes[i], NULL);
    }
    ui_perf_reset(&s_perf, now_ms());
}
