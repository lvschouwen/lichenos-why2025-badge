// Stress (#278): next to Bench, which answers "how fast" in ~10 s, this answers "does everything hold under concurrent
// load for minutes, and how deep do memory and stacks go". The run is stress_workers.c's; this is its screen, its tile
// and `fw stress [minutes|stop|status]`. One path for the run's lines: ESP_LOG with the tag "stress" (UART and log
// store, wherever the run was started). The console only acknowledges ("stress started ...", "stress status ...")
// and never repeats a "stress:" line, so the UART shows each one once.
#include "app_icons.h"
#include "fonts.h"
#include "fw_console.h"
#include "lichen_app.h"
#include "lichen_ui.h"
#include "shell_parts.h"
#include "stress_workers.h"
#include "theme.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#define REFRESH_MS 500
#define SCROLL_STEP 96
#define KB(b)      ((unsigned)((b) / 1024))
#define STOP_FOR_UPDATE_MS 10000

enum { ROW_TIME, ROW_RESULT, ROW_DMA, ROW_INT, ROW_PSRAM, ROW_POOLS, ROW_LAT, ROW_WORK, ROW_ERRORS, ROWS };
static char const *const KEYS[ROWS] = {"Time", "Result", "DMA RAM", "Internal", "PSRAM", "Wi-Fi pools", "Latency",
                                       "Work", "Errors"};

typedef struct {
    lv_obj_t   *screen;
    lv_obj_t   *value[ROWS];
    lv_timer_t *timer;
    char        refused[64]; // why the last Enter did not start a run, "" when it did
    bool        was_running;
} stress_screen_t;

static stress_screen_t s_s; // LVGL task only

static uint32_t errors_of(stress_counts_t const *c) {
    return c->dma_bad + c->mismatches + c->alloc_fails + c->heap_bad + c->lock_hits + c->sd_io + c->fs_io;
}

// "45 s", "10 min", "3 min 20 s"
static void fmt_time(char *out, size_t cap, uint32_t s) {
    if (s < 60) {
        snprintf(out, cap, "%u s", (unsigned)s);
    } else if (s % 60 == 0) {
        snprintf(out, cap, "%u min", (unsigned)(s / 60));
    } else {
        snprintf(out, cap, "%u min %u s", (unsigned)(s / 60), (unsigned)(s % 60));
    }
}

static void result_text(stress_view_t const *v, char *out, size_t cap) {
    if (v->running) {
        snprintf(out, cap, "Running");
    } else if (s_s.refused[0] || !v->ended) {
        snprintf(out, cap, "%s", s_s.refused[0] ? s_s.refused : "Not run yet");
    } else if (stress_failed(v->run.reason)) {
        snprintf(out, cap, "Failed: %s at %u s", stress_reason_name(v->run.reason), (unsigned)v->run.stop_s);
    } else if (v->run.reason != STRESS_NONE) {
        snprintf(out, cap, "Passed until stopped (%s)", stress_reason_name(v->run.reason));
    } else {
        snprintf(out, cap, "Passed");
    }
}

static void set_row(int row, char const *text) {
    if (strcmp(lv_label_get_text(s_s.value[row]), text) != 0) {
        lv_label_set_text(s_s.value[row], text);
    }
}

// The figures rows: the newest sample and the run's lowest (or highest) value; "-" before the first sample. One
// buffer at a time: this runs on the LVGL task
static void figure_rows(stress_view_t const *v) {
    char                   t[48];
    stress_sample_t const *s = &v->last, *lo = &v->run.lo;
    unsigned               ps10 = (unsigned)(((uint64_t)s->psram_free * 10 + 512 * 1024) / (1024 * 1024));
    for (int row = ROW_DMA; row <= ROW_LAT; row++) {
        switch (row) {
        case ROW_DMA: snprintf(t, sizeof(t), "%u KB free, lowest %u KB", KB(s->dma_free), KB(lo->dma_free)); break;
        case ROW_INT: snprintf(t, sizeof(t), "%u KB free, lowest %u KB", KB(s->int_free), KB(lo->int_free)); break;
        case ROW_PSRAM: snprintf(t, sizeof(t), "%u.%u MB free", ps10 / 10, ps10 % 10); break;
        case ROW_POOLS: snprintf(t, sizeof(t), "peak %u, %u dropped", (unsigned)lo->pool_peak, (unsigned)lo->pool_drop); break;
        default: snprintf(t, sizeof(t), "worst %u ms", (unsigned)lo->lat_ms); break;
        }
        set_row(row, v->run.have ? t : "-");
    }
}

static uint32_t result_colour(stress_view_t const *v) {
    if (v->running) {
        return THEME_TEXT;
    }
    if (s_s.refused[0] || (v->ended && stress_failed(v->run.reason))) {
        return THEME_RED;
    }
    return v->ended ? THEME_TEAL : THEME_DIM;
}

static void work_row(stress_view_t const *v) {
    char t[96], sd[16] = "no card", fs[16] = "no drive";
    if (v->sd) {
        snprintf(sd, sizeof(sd), "%u", (unsigned)v->counts.sd_cycles);
    }
    if (v->fs) {
        snprintf(fs, sizeof(fs), "%u", (unsigned)v->counts.fs_cycles);
    }
    snprintf(t, sizeof(t), "DMA %u, SD %s, files %s", (unsigned)v->counts.dma_rounds, sd, fs);
    set_row(ROW_WORK, t);
}

static void refresh(void) {
    stress_view_t v;
    stress_view(&v);
    char t[96], a[24], b[24];
    fmt_time(a, sizeof(a), v.t_s);
    fmt_time(b, sizeof(b), v.minutes * 60u);
    snprintf(t, sizeof(t), "%s of %s", a, b);
    set_row(ROW_TIME, v.minutes ? t : "-");
    result_text(&v, t, sizeof(t));
    set_row(ROW_RESULT, t);
    lv_obj_set_style_text_color(s_s.value[ROW_RESULT], lv_color_hex(result_colour(&v)), 0);
    figure_rows(&v);
    work_row(&v);
    uint32_t errors = errors_of(&v.counts);
    snprintf(t, sizeof(t), "%u (the log has each one)", (unsigned)errors);
    set_row(ROW_ERRORS, errors ? t : "None");
    if (v.running != s_s.was_running) {
        shell_page_hint(s_s.screen, v.running ? "Esc" : "Enter", v.running ? "stop" : "start 10 min", NULL);
        s_s.was_running = v.running;
    }
}

static void on_timer(lv_timer_t *timer) {
    (void)timer;
    refresh();
}

static void start_from_screen(void) {
    char const *why = stress_start(STRESS_MIN_DEFAULT);
    if (why) {
        snprintf(s_s.refused, sizeof(s_s.refused), "Not started: %s", why);
    } else {
        s_s.refused[0] = '\0';
    }
    refresh();
}

static void on_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        shell_scroll_by(lv_event_get_target(e), key == LV_KEY_UP ? SCROLL_STEP : -SCROLL_STEP); // only when it does not fit
    } else if (key == LV_KEY_ENTER && !stress_running()) {
        start_from_screen();
    }
}

// A running stress is a mode of this screen (R6): Esc stops it first and stays, the next Esc goes back
static bool on_esc(void *arg) {
    (void)arg;
    if (!stress_running()) {
        return false;
    }
    stress_stop(STRESS_ESC);
    return true;
}

static void on_delete(lv_event_t *e) {
    (void)e;
    lv_timer_delete(s_s.timer);
    s_s = (stress_screen_t){0};
}

static int32_t key_col_w(lv_obj_t *obj) {
    lv_font_t const *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    int32_t          w    = 0;
    for (size_t i = 0; i < ROWS; i++) {
        lv_point_t size;
        lv_text_get_size(&size, KEYS[i], font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        w = size.x > w ? size.x : w;
    }
    return w + 24;
}

static lv_obj_t *add_row(lv_obj_t *parent, char const *key, int32_t key_w) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(row, false);
    lv_obj_t *k = lv_label_create(row);
    lv_obj_set_width(k, key_w);
    lv_obj_set_style_text_color(k, lv_color_hex(THEME_DIM), 0);
    lv_label_set_text(k, key);
    lv_obj_t *v = lichen_label_wrap(row, 0);
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_text(v, "");
    return v;
}

static void open_stress(lv_obj_t *screen) {
    s_s        = (stress_screen_t){.screen = screen, .was_running = !stress_running()};
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 12, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUS_KEY);
    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_font(title, lichen_font(LICHEN_FONT_LARGE), 0);
    lv_label_set_text(title, "Stress");
    lv_obj_t *sub = lichen_label_wrap(screen, LV_PCT(100));
    lv_obj_set_style_text_color(sub, lv_color_hex(THEME_DIM), 0);
    lv_label_set_text(sub, "DMA, PSRAM, SD card, storage and redraws at once, with a latency probe. Nothing is sent "
                           "on air.");
    int32_t key_w = key_col_w(screen);
    for (int row = 0; row < ROWS; row++) {
        s_s.value[row] = add_row(screen, KEYS[row], key_w);
    }
    s_s.timer = lv_timer_create(on_timer, REFRESH_MS, NULL);
    lv_obj_add_event_cb(screen, on_key, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    shell_on_esc(on_esc, NULL);
    refresh();
}

static bool stress_tile(char *l1, size_t c1, char *l2, size_t c2) {
    stress_view_t v;
    stress_view(&v);
    snprintf(l1, c1, "Stress");
    if (v.running) {
        snprintf(l2, c2, "%u of %u min", (unsigned)(v.t_s / 60), v.minutes);
    } else if (v.ended) {
        snprintf(l2, c2, "%s", stress_failed(v.run.reason) ? "Failed" : "Passed");
    } else {
        snprintf(l2, c2, "Enter: run");
    }
    return true;
}

// An update stops everything: the run ends (its files are deleted) before the image arrives
static bool stress_stop_for_update(void) {
    stress_stop(STRESS_UPDATE);
    for (int ms = 0; stress_running() && ms < STOP_FOR_UPDATE_MS; ms += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return !stress_running();
}

static void cmd_status(void) {
    stress_view_t v;
    stress_view(&v);
    if (!v.running && !v.ended) {
        console_reply("stress status: no run since boot");
        return;
    }
    console_reply("stress status: %s %u s of %u min, DMA lowest %u KB, latency worst %u ms, %u error(s)%s%s",
                  v.running ? "running" : "ended", (unsigned)v.t_s, v.minutes, KB(v.run.lo.dma_free),
                  (unsigned)v.run.lo.lat_ms, (unsigned)errors_of(&v.counts), v.run.reason ? ", reason " : "",
                  v.run.reason ? stress_reason_name(v.run.reason) : "");
}

static void cmd_stress(char const *args) {
    unsigned minutes;
    switch (stress_parse(args, &minutes)) {
    case STRESS_CMD_START: {
        char const *why = stress_start(minutes);
        if (why) {
            console_reply("stress refused: %s", why);
        } else {
            console_reply("stress started, %u min: the lines follow in the log (tag stress)", minutes);
        }
        break;
    }
    case STRESS_CMD_STOP:
        console_reply(stress_running() ? "stress stopping: the report follows in the log" : "stress status: no run on");
        stress_stop(STRESS_STOP_CMD);
        break;
    case STRESS_CMD_STATUS: cmd_status(); break;
    case STRESS_CMD_BAD: console_reply("stress [1-%d minutes, default %d | stop | status]", STRESS_MIN_MAX, STRESS_MIN_DEFAULT); break;
    }
}

static lichen_app_t const stress_app = {
    .name            = "Stress",
    .id              = "stress",
    .draw_icon       = icon_stress,
    .tile_text       = stress_tile,
    .open            = open_stress,
    .stop_for_update = stress_stop_for_update,
};

void stress_app_register(void) {
    lichen_register_app(&stress_app);
    console_register("stress", "stress [minutes 1-30 | stop | status] (#278: DMA, PSRAM, SD, storage, redraw and latency "
                               "load at once; a \"stress:\" log line every 10 s, then the report; never transmits)",
                     cmd_stress);
}
