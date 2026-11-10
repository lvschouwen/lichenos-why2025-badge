// The stress run's console and screen (#278). Console only: `fw stress [1-30|stop|status]` is the one way to start it;
// the badge has no tile for it. The run (stress_workers.c) locks the physical keys (#190) and opens this screen over
// whatever was open; the screen is also the UI load (it redraws itself every 100 ms while the run is on). At the end it
// shows PASS or FAIL and the keys are free again: any key, or `fw stress status`, goes back to the screen before.
// One path for the run's lines: ESP_LOG with the tag "stress" (UART and log store). The console only acknowledges
// ("stress started ...", "stress status: ..."), never repeating a "stress:" line, so the UART shows each one once.
#include "fonts.h"
#include "fw_console.h"
#include "lichen_sys_ui.h"
#include "lichen_ui.h"
#include "nav.h"
#include "remote_ctl.h"
#include "screen_ids.h"
#include "shell_parts.h"
#include "stress_workers.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define TICK_MS        100 // the UI load: one full redraw of this screen per tick
#define REFRESH_TICKS  5   // the figures change every 0.5 s
#define BULLET         " \xE2\x80\xA2 "

typedef struct {
    lv_obj_t   *screen, *title, *sub, *clock, *bar, *workers, *dma, *result;
    lv_timer_t *timer;
    unsigned    ticks;
    bool        done; // the result is shown
} stress_page_t;

static stress_page_t s_p; // LVGL task only

static void refresh(stress_view_t const *v) {
    char text[200];
    stress_fmt_clock(text, sizeof(text), v->t_s, v->minutes * 60u);
    lv_label_set_text(s_p.clock, text);
    lv_bar_set_value(s_p.bar, v->minutes ? (int32_t)(v->t_s * 100u / (v->minutes * 60u)) : 0, LV_ANIM_OFF);
    unsigned failed = v->fail | (v->counts.n[STRESS_C_LOCK] ? STRESS_W_UI : 0) |
                      (v->run.reason == STRESS_LATENCY ? STRESS_W_LAT : 0);
    unsigned skipped = (v->sd ? 0 : STRESS_W_SD) | (v->fs ? 0 : STRESS_W_FS);
    stress_fmt_workers(text, sizeof(text), &v->counts, failed, skipped, v->run.lo.lat_ms);
    lv_label_set_text(s_p.workers, text);
    if (v->run.have) {
        lv_label_set_text_fmt(s_p.dma, "DMA min %u KB", (unsigned)(v->run.lo.dma_free / 1024));
    }
}

static void show_result(stress_view_t const *v) {
    bool pass = !stress_failed(v->run.reason);
    lv_label_set_text(s_p.title, "Stress test done");
    lv_label_set_text(s_p.sub, "started from the console" BULLET "any key: back");
    if (pass) {
        lv_label_set_text(s_p.result, "PASS");
    } else {
        lv_label_set_text_fmt(s_p.result, "FAIL: %s", stress_reason_name(v->run.reason));
    }
    lv_obj_set_style_text_color(s_p.result, lv_color_hex(pass ? THEME_TEAL : THEME_RED), 0);
    s_p.done = true;
}

static void tick(lv_timer_t *t) {
    (void)t;
    stress_view_t v;
    stress_view(&v);
    if (v.running) {
        lv_obj_invalidate(s_p.screen);
        stress_ui_frame();
    }
    if (++s_p.ticks % REFRESH_TICKS == 0 || (v.ended && !s_p.done)) {
        refresh(&v);
    }
    if (v.ended && !v.running && !s_p.done) {
        show_result(&v);
    }
}

// Once the result is up, any key goes back (Esc does so itself)
static void on_key(lv_event_t *e) {
    if (s_p.done && lv_event_get_key(e) != LV_KEY_ESC) {
        shell_back();
    }
}

// While the run is on the keys are locked; an Esc that gets through (the lock just ended) must not leave the screen
static bool on_esc(void *arg) {
    (void)arg;
    return !s_p.done;
}

static void on_delete(lv_event_t *e) {
    (void)e;
    lv_timer_delete(s_p.timer);
    s_p = (stress_page_t){0};
}

// How to stop is Esc held RLOCK_ESC_HOLD_MS (3 s): the run holds the key lock and ends when it does (UI rule 6, #286)
#define STRESS_SUB "keys are locked" BULLET "hold Esc 3 s to stop"
_Static_assert(RLOCK_ESC_HOLD_MS == 3000, "the stress page's hint names the key lock's Esc hold");

static lv_obj_t *add_label(lv_obj_t *parent, int font, uint32_t colour) {
    lv_obj_t *l = lichen_label_wrap(parent, LV_PCT(100));
    lv_obj_set_style_text_font(l, lichen_font(font), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *build_stress(void) {
    lv_obj_t *screen = shell_page_create();
    s_p              = (stress_page_t){.screen = screen};
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 14, 0);
    s_p.title = add_label(screen, LICHEN_FONT_LARGE, THEME_TEXT);
    lv_label_set_text(s_p.title, "Stress test running");
    s_p.sub = add_label(screen, LICHEN_FONT_SMALL, THEME_DIM);
    lv_label_set_text(s_p.sub, STRESS_SUB);
    s_p.clock = add_label(screen, LICHEN_FONT_MEDIUM, THEME_TEXT);
    s_p.bar   = lv_bar_create(screen);
    lv_obj_set_size(s_p.bar, LV_PCT(100), 16);
    lv_obj_set_style_bg_color(s_p.bar, lv_color_hex(THEME_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_p.bar, lv_color_hex(THEME_TEAL), LV_PART_INDICATOR);
    s_p.workers = add_label(screen, LICHEN_FONT_SMALL, THEME_TEXT);
    s_p.dma     = add_label(screen, LICHEN_FONT_SMALL, THEME_DIM);
    lv_label_set_text(s_p.dma, "DMA min -");
    s_p.result = add_label(screen, LICHEN_FONT_LARGE, THEME_TEXT);
    s_p.timer  = lv_timer_create(tick, TICK_MS, NULL);
    lv_obj_add_event_cb(screen, on_key, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    shell_on_esc(on_esc, NULL);
    stress_view_t v;
    stress_view(&v);
    refresh(&v);
    return screen;
}

void stress_screen_open(void) {
    shell_open(build_stress, SCREEN_STRESS); // a screen still up from the last run is reused, its state reset below
    lichen_sys_ui_lock(0);
    if (s_p.screen && s_p.done) { // the result of the run before: back to "running"
        lv_label_set_text(s_p.title, "Stress test running");
        lv_label_set_text(s_p.sub, STRESS_SUB);
        lv_label_set_text(s_p.result, "");
        s_p.done = false;
    }
    lichen_sys_ui_unlock();
}

// `fw stress status` also takes a finished run's result off the screen
static void close_result(void *arg) {
    (void)arg;
    if (s_p.done && nav_current() == SCREEN_STRESS) {
        shell_back();
    }
}

static void cmd_status(void) {
    stress_view_t v;
    stress_view(&v);
    if (!v.running && !v.ended) {
        console_reply("stress status: no run since boot");
        return;
    }
    console_reply("stress status: %s %u s of %u min, DMA lowest %u KB, latency worst %u ms, %u error(s), reason %s",
                  v.running ? "running" : "ended", (unsigned)v.t_s, v.minutes, (unsigned)(v.run.lo.dma_free / 1024),
                  (unsigned)v.run.lo.lat_ms, (unsigned)stress_errors(&v.counts), stress_reason_name(v.run.reason));
    lichen_sys_ui_call(close_result, NULL, 2000);
}

static void cmd_stress(char const *args) {
    unsigned    minutes;
    char const *why;
    switch (stress_parse(args, &minutes)) {
    case STRESS_CMD_START:
        why = stress_start(minutes);
        if (why) {
            console_reply("stress refused: %s", why);
        } else {
            console_reply("stress started, %u min: the lines follow in the log (tag stress)", minutes);
        }
        break;
    case STRESS_CMD_STOP:
        console_reply(stress_running() ? "stress stopping: the report follows in the log" : "stress status: no run on");
        stress_stop(STRESS_STOP_CMD);
        break;
    case STRESS_CMD_STATUS: cmd_status(); break;
    case STRESS_CMD_BAD: console_reply("stress [1-30 minutes, default 10 | stop | status]"); break;
    }
}

void stress_init(void) {
    console_register("stress", "stress [minutes 1-30 | stop | status] (#278: DMA, PSRAM, SD, storage, redraw and latency "
                               "load at once, keys locked; a \"stress:\" log line every 10 s, then the report; never "
                               "transmits)",
                     cmd_stress);
}
