// Bench (#96): the badge's own numbers on one screen, so a PSRAM clock (#94) or a display path (#95) can be compared
// without a console walk. A run is a stage machine on an LVGL timer, so the screen shows each result as it lands:
// PSRAM write / read / copy / internal-to-PSRAM over 1 MB with the cache dropped, ten full redraws of this screen, a
// synthetic 100-row list scrolled for three seconds, then memory. Enter runs it again; every result is also one
// "bench" log line and the run ends with a one-line summary, so `badge.sh log` keeps the baseline. "fw bench" runs the
// same minus the scroll stage from the console.
#include "app_icons.h"
#include "bench_calc.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "fonts.h"
#include "fw_console.h"
#include "lichen_app.h"
#include "lichen_ui.h"
#include "psram_boot.h"
#include "sdkconfig.h"
#include "shell_parts.h"
#include "theme.h"
#include "ui_perf.h"

#include <stdio.h>
#include <string.h>

#define BYTES       BENCH_MB
#define INT_CHUNK   (32 * 1024)
#define REDRAWS     10
#define SCROLL_MS   3000
#define SCROLL_STEP 40
#define TICK_MS     50
#define LIST_ROWS   100
#define SCROLL_STEP_UI 96
#define VALUE_CHARS 96

static char const TAG[] = "bench";

enum { ROW_PSRAM, ROW_WRITE, ROW_READ, ROW_COPY, ROW_INT, ROW_REDRAW, ROW_SCROLL, ROW_MEM, ROWS };
static char const *const KEYS[ROWS] = {"PSRAM", "write", "read", "copy", "int" LV_SYMBOL_RIGHT "PSRAM", "redraw", "scroll", "memory"};

typedef enum { ST_IDLE, ST_BANDWIDTH, ST_REDRAW, ST_SCROLL_START, ST_SCROLL, ST_MEM } stage_t;

typedef struct {
    uint64_t write_us, read_us, copy_us, int_us;
} bw_t;

typedef struct {
    lv_obj_t   *screen;
    lv_obj_t   *value[ROWS];
    lv_obj_t   *hint;
    lv_obj_t   *list; // the synthetic list while the scroll stage runs
    lv_timer_t *timer;
    stage_t     stage;
    char        result[ROWS][VALUE_CHARS];
    int64_t     scroll_t0;
    int32_t     scroll_y, scroll_max;
    ui_perf_t   before;
    unsigned    runs;
} bench_t;

static bench_t          s_b;
static char             s_last[40]; // the tile's second line: the last redraw figure
static volatile uint32_t s_sink;    // keeps the read loop honest

// ---- measurements: no LVGL, any task ---------------------------------------------------------------------------------

static void drop_cache(void *p, size_t n) {
    esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

// Four passes over 1 MB of PSRAM: stores pushed out to the RAM, loads that miss the (256 KB) cache, a PSRAM-to-PSRAM
// memcpy and a copy from a 32 KB internal chunk. false = no memory for the buffers.
static bool bandwidth_run(bw_t *out) {
    uint8_t *a  = heap_caps_aligned_alloc(128, BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *b  = heap_caps_aligned_alloc(128, BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *in = heap_caps_malloc(INT_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!a || !b || !in) {
        heap_caps_free(a);
        heap_caps_free(b);
        heap_caps_free(in);
        return false;
    }
    memset(in, 0x5A, INT_CHUNK);
    uint32_t *w  = (uint32_t *)a;
    int64_t   t0 = esp_timer_get_time();
    for (size_t i = 0; i < BYTES / 4; i++) {
        w[i] = (uint32_t)i * 0x9E3779B9u;
    }
    drop_cache(a, BYTES);
    int64_t t1 = esp_timer_get_time();
    uint32_t sum = 0;
    for (size_t i = 0; i < BYTES / 4; i++) {
        sum += w[i];
    }
    s_sink     = sum;
    int64_t t2 = esp_timer_get_time();
    memcpy(b, a, BYTES);
    drop_cache(b, BYTES);
    int64_t t3 = esp_timer_get_time();
    for (size_t off = 0; off < BYTES; off += INT_CHUNK) {
        memcpy(b + off, in, INT_CHUNK);
    }
    drop_cache(b, BYTES);
    int64_t t4 = esp_timer_get_time();
    heap_caps_free(a);
    heap_caps_free(b);
    heap_caps_free(in);
    *out = (bw_t){.write_us = (uint64_t)(t1 - t0), .read_us = (uint64_t)(t2 - t1), .copy_us = (uint64_t)(t3 - t2), .int_us = (uint64_t)(t4 - t3)};
    return true;
}

// Ten full redraws of the active screen; the flush share from the display counters. LVGL context.
static void redraw_run(char *out, size_t cap) {
    ui_perf_t p0, p1;
    ui_perf_snapshot(&p0);
    uint64_t us = ui_perf_redraw_us(REDRAWS);
    ui_perf_snapshot(&p1);
    bench_fmt_redraw(out, cap, us, REDRAWS, p1.flush.sum_us - p0.flush.sum_us, p1.flush.n - p0.flush.n);
}

static void mem_line(char *out, size_t cap) {
    multi_heap_info_t in;
    heap_caps_get_info(&in, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bench_fmt_mem(out, cap, in.total_free_bytes, in.minimum_free_bytes, heap_caps_get_free_size(MALLOC_CAP_DMA),
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void psram_line(char *out, size_t cap) {
    int n = snprintf(out, cap, "%u MHz, ", (unsigned)CONFIG_SPIRAM_SPEED);
    if (n > 0 && (size_t)n < cap) {
        psram_boot_tuning_text(out + n, cap - (size_t)n);
    }
}

// ---- the screen -----------------------------------------------------------------------------------------------------

static void set_result(int row, char const *text) {
    strlcpy(s_b.result[row], text, VALUE_CHARS);
    if (s_b.value[row]) {
        lv_label_set_text(s_b.value[row], text);
    }
    ESP_LOGW(TAG, "%s: %s", KEYS[row], text);
}

static void set_hint(char const *text) {
    if (s_b.hint) {
        lv_label_set_text(s_b.hint, text);
    }
}

// Two lines: the log ring cuts a line at about 200 characters, the tuning text alone is 80
static void summary(void) {
    ESP_LOGW(TAG, "run %u psram: %s | write %s | read %s | copy %s | int->psram %s", s_b.runs, s_b.result[ROW_PSRAM],
             s_b.result[ROW_WRITE], s_b.result[ROW_READ], s_b.result[ROW_COPY], s_b.result[ROW_INT]);
    ESP_LOGW(TAG, "run %u ui: redraw %s | scroll %s | %s", s_b.runs, s_b.result[ROW_REDRAW], s_b.result[ROW_SCROLL],
             s_b.result[ROW_MEM]);
    strlcpy(s_last, "redraw ", sizeof(s_last));
    strlcat(s_last, s_b.result[ROW_REDRAW], sizeof(s_last));
    char *comma = strchr(s_last, ',');
    if (comma) {
        *comma = '\0';
    }
}

// A list shaped like MeshCore's contacts: 100 rows of a name and a dim detail line, over the results
static lv_obj_t *make_list(lv_obj_t *screen) {
    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_add_flag(list, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(list, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 12, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    shell_scroll_tame(list);
    for (unsigned i = 0; i < LIST_ROWS; i++) {
        char text[48];
        lv_obj_t *name = lv_label_create(list);
        lv_obj_set_width(name, LV_PCT(100));
        lv_obj_set_style_text_font(name, lichen_font(LICHEN_FONT_MEDIUM), 0);
        snprintf(text, sizeof(text), "NL-BENCH-NODE-%03u", i);
        lv_label_set_text(name, text);
        lv_obj_t *detail = lv_label_create(list);
        lv_obj_set_width(detail, LV_PCT(100));
        lv_obj_set_style_text_color(detail, lv_color_hex(THEME_DIM), 0);
        snprintf(text, sizeof(text), "repeater, flood, %u min ago", i * 3 + 1);
        lv_label_set_text(detail, text);
    }
    lv_obj_update_layout(list);
    return list;
}

static void tick(lv_timer_t *timer) {
    (void)timer;
    int64_t now = esp_timer_get_time();
    switch (s_b.stage) {
        case ST_IDLE: lv_timer_pause(s_b.timer); break;
        case ST_BANDWIDTH: {
            char text[VALUE_CHARS];
            psram_line(text, sizeof(text));
            set_result(ROW_PSRAM, text);
            bw_t r;
            if (bandwidth_run(&r)) {
                bench_fmt_rate(text, sizeof(text), BYTES, r.write_us);
                set_result(ROW_WRITE, text);
                bench_fmt_rate(text, sizeof(text), BYTES, r.read_us);
                set_result(ROW_READ, text);
                bench_fmt_rate(text, sizeof(text), BYTES, r.copy_us);
                set_result(ROW_COPY, text);
                bench_fmt_rate(text, sizeof(text), BYTES, r.int_us);
                set_result(ROW_INT, text);
            } else {
                for (int row = ROW_WRITE; row <= ROW_INT; row++) {
                    set_result(row, "no memory for 1 MB");
                }
            }
            set_hint("running: redraw");
            s_b.stage = ST_REDRAW;
            break;
        }
        case ST_REDRAW: {
            char text[VALUE_CHARS];
            redraw_run(text, sizeof(text));
            set_result(ROW_REDRAW, text);
            set_hint("running: scroll");
            s_b.stage = ST_SCROLL_START;
            break;
        }
        case ST_SCROLL_START:
            s_b.list       = make_list(s_b.screen);
            s_b.scroll_max = lv_obj_get_scroll_bottom(s_b.list);
            s_b.scroll_y   = 0;
            ui_perf_snapshot(&s_b.before);
            s_b.scroll_t0 = now;
            s_b.stage     = ST_SCROLL;
            break;
        case ST_SCROLL:
            if (now - s_b.scroll_t0 < (int64_t)SCROLL_MS * 1000) {
                s_b.scroll_y += SCROLL_STEP;
                if (s_b.scroll_y > s_b.scroll_max) {
                    s_b.scroll_y = 0;
                }
                lv_obj_scroll_to_y(s_b.list, s_b.scroll_y, LV_ANIM_OFF);
            } else {
                ui_perf_t after;
                ui_perf_snapshot(&after);
                unsigned frames = after.render.n - s_b.before.render.n;
                uint32_t avg    = frames ? (uint32_t)((after.render.sum_us - s_b.before.render.sum_us) / frames) : 0;
                char     text[VALUE_CHARS];
                bench_fmt_scroll(text, sizeof(text), frames, (uint32_t)((now - s_b.scroll_t0) / 1000), avg);
                set_result(ROW_SCROLL, text);
                lv_obj_delete(s_b.list);
                s_b.list  = NULL;
                s_b.stage = ST_MEM;
            }
            break;
        case ST_MEM: {
            char text[VALUE_CHARS];
            mem_line(text, sizeof(text));
            set_result(ROW_MEM, text);
            s_b.runs++;
            summary();
            set_hint("Enter: run again    Esc: back");
            s_b.stage = ST_IDLE;
            lv_timer_pause(s_b.timer);
            break;
        }
    }
}

static void start_run(void) {
    if (s_b.stage != ST_IDLE) {
        return;
    }
    for (int row = ROW_WRITE; row < ROWS; row++) {
        lv_label_set_text(s_b.value[row], "...");
    }
    set_hint("running: PSRAM bandwidth");
    s_b.stage = ST_BANDWIDTH;
    lv_timer_resume(s_b.timer);
}

// Enter arrives as a key event here, as on home and in menus
static void on_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        shell_scroll_by(lv_event_get_target(e), key == LV_KEY_UP ? SCROLL_STEP_UI : -SCROLL_STEP_UI);
    } else if (key == LV_KEY_ENTER) {
        start_run();
    }
}

static void on_delete(lv_event_t *e) {
    (void)e;
    lv_timer_delete(s_b.timer);
    s_b = (bench_t){0}; // the list, if any, went with the screen
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
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(row, false);
    lv_obj_t *k = lv_label_create(row);
    lv_obj_set_width(k, key_w);
    lv_obj_set_style_text_color(k, lv_color_hex(THEME_DIM), 0);
    lv_label_set_text(k, key);
    lv_obj_t *v = lv_label_create(row);
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_text(v, "");
    return v;
}

static void open_bench(lv_obj_t *screen) {
    s_b        = (bench_t){0};
    s_b.screen = screen;
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 16, 0);
    lv_obj_set_style_pad_bottom(screen, THEME_PAD, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUS_KEY);

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_font(title, lichen_font(LICHEN_FONT_LARGE), 0);
    lv_label_set_text(title, "Bench");
    lv_obj_t *sub = lv_label_create(screen);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_obj_set_style_text_color(sub, lv_color_hex(THEME_DIM), 0);
    lv_label_set_text(sub, "1 MB PSRAM passes (cache dropped), 10 redraws of this screen, a 100-row list scrolled "
                           "for 3 s. Every number is also a log line.");

    int32_t key_w = key_col_w(screen);
    for (int row = 0; row < ROWS; row++) {
        s_b.value[row] = add_row(screen, KEYS[row], key_w);
    }
    s_b.hint = lv_label_create(screen);
    lv_obj_set_style_text_color(s_b.hint, lv_color_hex(THEME_AMBER), 0);
    lv_label_set_text(s_b.hint, "");

    s_b.timer = lv_timer_create(tick, TICK_MS, NULL);
    lv_timer_pause(s_b.timer);
    lv_obj_add_event_cb(screen, on_key, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    start_run();
}

static bool bench_tile(char *l1, size_t c1, char *l2, size_t c2) {
    snprintf(l1, c1, "PSRAM %u MHz", (unsigned)CONFIG_SPIRAM_SPEED);
    snprintf(l2, c2, "%s", s_last[0] ? s_last : "Enter: run");
    return true;
}

// "fw bench": the bandwidth, redraw and memory figures from the console; the current screen is the one redrawn
static void cmd_bench(char const *args) {
    (void)args;
    char text[VALUE_CHARS];
    psram_line(text, sizeof(text));
    console_reply("bench psram: %s", text);
    bw_t r;
    if (bandwidth_run(&r)) {
        bench_fmt_rate(text, sizeof(text), BYTES, r.write_us);
        console_reply("bench write 1 MB: %s", text);
        bench_fmt_rate(text, sizeof(text), BYTES, r.read_us);
        console_reply("bench read 1 MB: %s", text);
        bench_fmt_rate(text, sizeof(text), BYTES, r.copy_us);
        console_reply("bench copy 1 MB psram->psram: %s", text);
        bench_fmt_rate(text, sizeof(text), BYTES, r.int_us);
        console_reply("bench copy 1 MB internal->psram: %s", text);
    } else {
        console_reply("bench: no memory for the 1 MB buffers");
    }
    lvgl_port_lock(0);
    redraw_run(text, sizeof(text));
    lvgl_port_unlock();
    console_reply("bench redraw (current screen, %u frames): %s", REDRAWS, text);
    mem_line(text, sizeof(text));
    console_reply("bench memory: %s", text);
}

static lichen_app_t const bench_app = {
    .name      = "Bench",
    .id        = "bench",
    .draw_icon = icon_bench,
    .tile_text = bench_tile,
    .open      = open_bench,
};

void bench_app_register(void) {
    lichen_register_app(&bench_app);
    console_register("bench", "bench (PSRAM bandwidth, full redraws of the current screen, memory; the Bench app adds the scroll test)", cmd_bench);
}
