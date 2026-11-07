// The station clock view (#119, #122): a Swiss railway style dial, white with black bars, black hands and a red
// stop-to-go second hand (station_clock.c has the times and the geometry). The 720x720 panel is flushed by the CPU and
// turned in software, so the sweep must not redraw the screen: the dial is drawn once into a canvas in PSRAM (an
// image blit per redraw, no 60 anti-aliased bars), the hands are drawn on top of it in the canvas's draw event, and a
// ~15 fps timer invalidates only the second hand's old and new places, split along the hand so a diagonal hand is a
// few small boxes instead of one big one. The minute and hour hands are invalidated once a minute, when they move.
// No drift (unlike the seven-segment clock): the dial fills its area and the panel is an LCD.
// Shared by the screensaver and the Clock app (through lichen_station_clock, lichen_ui.h), which can be up at the same
// time, so all state is in a station_view_t.
// Everything here runs on the LVGL task (the timer and the callers' builds).
#include "station_view.h"

#include "lichen_sys_mem.h"
#include "lichen_mem.h"
#include "esp_log.h"
#include "fonts.h"
#include "lichen_clock.h"
#include "lichen_ui.h"
#include "theme.h"
#include "shell_ui_ops.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static char const TAG[] = "station";

#define FRAME_MS      66 // ~15 fps: the second hand moves ~0.4 degrees, ~1.2 px at the disc, a frame
#define PAD           2  // the anti-aliased edge around a redrawn hand
#define SECOND_PIECES 4
#define HAND_PIECES   2

#define DIAL_WHITE 0xFFFFFF
#define HAND_BLACK 0x111111
#define SIGNAL_RED 0xD8101C

typedef struct {
    bool    set; // no hands while the clock is not set
    int32_t hour, minute, second;
} hands_t;

struct station_view {
    lv_obj_t   *dial;
    uint8_t    *dial_buf;
    lv_obj_t   *date_label, *line2;
    lv_timer_t *timer;
    sc_dims_t   dims;
    int32_t     d, r;   // the dial's diameter and radius
    int32_t     cx, cy; // the dial's centre, screen coordinates
    hands_t     shown;  // what the dial shows (the draw event draws this, the timer moves it)
    time_t      labels_at;
    void (*update_line2)(lv_obj_t *line2);
};

static void draw_bar(lv_layer_t *layer, int32_t x, int32_t y, int32_t a, sc_bar_t bar, uint32_t color) {
    sc_point_t         p0, p1;
    lv_draw_line_dsc_t d;
    sc_bar_ends(x, y, a, bar, &p0, &p1);
    lv_draw_line_dsc_init(&d);
    d.p1.x  = p0.x;
    d.p1.y  = p0.y;
    d.p2.x  = p1.x;
    d.p2.y  = p1.y;
    d.width = bar.w;
    d.color = lv_color_hex(color);
    lv_draw_line(layer, &d);
}

static void draw_disc(lv_layer_t *layer, sc_point_t c, int32_t r, uint32_t color) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius    = LV_RADIUS_CIRCLE;
    d.bg_color  = lv_color_hex(color);
    lv_area_t a = {c.x - r, c.y - r, c.x + r, c.y + r};
    lv_draw_rect(layer, &d, &a);
}

// The white face and its 60 bars, once, into the canvas (canvas coordinates)
static bool dial_create(station_view_t *v, lv_obj_t *parent, sc_layout_t const *lay) {
    uint32_t stride = lv_draw_buf_width_to_stride(v->d, LV_COLOR_FORMAT_RGB565);
    v->dial_buf     = lichen_alloc_aligned(NULL, LV_DRAW_BUF_ALIGN, (size_t)stride * v->d, LICHEN_MEM_BULK);
    if (!v->dial_buf) {
        return false;
    }
    v->dial = lv_canvas_create(parent);
    lv_canvas_set_buffer(v->dial, v->dial_buf, v->d, v->d, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(v->dial, lv_color_black(), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(v->dial, &layer);
    lv_draw_rect_dsc_t face;
    lv_draw_rect_dsc_init(&face);
    face.radius   = LV_RADIUS_CIRCLE;
    face.bg_color = lv_color_hex(DIAL_WHITE);
    lv_area_t all = {0, 0, v->d - 1, v->d - 1};
    lv_draw_rect(&layer, &face, &all);
    for (int i = 0; i < 60; i++) {
        draw_bar(&layer, v->r, v->r, i * 60, i % 5 ? v->dims.minute_mark : v->dims.hour_mark, HAND_BLACK);
    }
    lv_canvas_finish_layer(v->dial, &layer);
    lv_obj_align(v->dial, LV_ALIGN_TOP_MID, 0, lay->dial_top);
    return true;
}

static void on_draw(lv_event_t *e) {
    station_view_t *v = lv_event_get_user_data(e);
    if (!v->shown.set) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    int32_t     cx = v->cx, cy = v->cy;
    draw_bar(layer, cx, cy, v->shown.hour, v->dims.hour, HAND_BLACK);
    draw_bar(layer, cx, cy, v->shown.minute, v->dims.minute, HAND_BLACK);
    draw_bar(layer, cx, cy, v->shown.second, v->dims.second, SIGNAL_RED);
    draw_disc(layer, sc_polar(cx, cy, v->shown.second, v->dims.second.r1), v->dims.disc_r, SIGNAL_RED);
    draw_disc(layer, (sc_point_t){cx, cy}, v->dims.cap_r, SIGNAL_RED);
}

static void invalidate_box(station_view_t *v, sc_box_t b) {
    lv_area_t a = {b.x1, b.y1, b.x2, b.y2};
    lv_obj_invalidate_area(v->dial, &a);
}

static void invalidate_bar(station_view_t *v, int32_t a, sc_bar_t bar, int32_t pieces) {
    int32_t len = bar.r1 - bar.r0;
    for (int32_t k = 0; k < pieces; k++) {
        invalidate_box(v, sc_bar_box(v->cx, v->cy, a, bar.w, bar.r0 + len * k / pieces, bar.r0 + len * (k + 1) / pieces, PAD));
    }
}

static void invalidate_second(station_view_t *v, int32_t a) {
    invalidate_bar(v, a, v->dims.second, SECOND_PIECES);
    invalidate_box(v, sc_disc_box(v->cx, v->cy, a, v->dims.second.r1, v->dims.disc_r, PAD));
}

static hands_t hands_now(struct timeval const *tv) {
    struct tm local;
    if (!clock_is_set() || !localtime_r(&tv->tv_sec, &local)) {
        return (hands_t){0};
    }
    // tm_sec 60 (a leap second) counts as the pause at 12
    uint32_t ms = (uint32_t)local.tm_sec * 1000u + (uint32_t)(tv->tv_usec / 1000);
    return (
        hands_t
    ){true, sc_hour_angle(local.tm_hour, local.tm_min), sc_minute_angle(local.tm_min), sc_second_angle(ms)};
}

bool saver_date_text(char *out, size_t cap) {
    time_t    now = time(NULL);
    struct tm local;
    if (!clock_is_set() || !localtime_r(&now, &local)) {
        snprintf(out, cap, "Clock not set");
        return false;
    }
    char day[16], month[16];
    strftime(day, sizeof(day), "%A", &local);
    strftime(month, sizeof(month), "%B", &local);
    snprintf(out, cap, "%s%s %d %s", clock_is_approximate() ? "~ " : "", day, local.tm_mday, month);
    return true;
}

static void labels_update(station_view_t *v) {
    char date[48];
    saver_date_text(date, sizeof(date));
    if (strcmp(lv_label_get_text(v->date_label), date) != 0) {
        lv_label_set_text(v->date_label, date);
    }
    if (v->update_line2) {
        v->update_line2(v->line2);
    }
}

static void frame(station_view_t *v) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    hands_t now = hands_now(&tv);
    if (now.set != v->shown.set) {
        lv_obj_invalidate(v->dial); // the hands appear or go: once
    } else if (now.set) {
        if (now.minute != v->shown.minute || now.hour != v->shown.hour) { // the jump on the minute
            invalidate_bar(v, v->shown.hour, v->dims.hour, HAND_PIECES);
            invalidate_bar(v, now.hour, v->dims.hour, HAND_PIECES);
            invalidate_bar(v, v->shown.minute, v->dims.minute, HAND_PIECES);
            invalidate_bar(v, now.minute, v->dims.minute, HAND_PIECES);
        }
        if (now.second != v->shown.second) { // nothing during the pause at 12
            invalidate_second(v, v->shown.second);
            invalidate_second(v, now.second);
        }
    }
    v->shown = now;
    if (tv.tv_sec != v->labels_at) {
        v->labels_at = tv.tv_sec;
        labels_update(v);
    }
}

static void tick(lv_timer_t *t) {
    frame(lv_timer_get_user_data(t));
}

static lv_obj_t *add_line(lv_obj_t *parent, int font, uint32_t color, int32_t y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, lichen_font(font), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, LV_PCT(100)); // centred whatever the text: the dial never moves
    lv_obj_set_pos(l, 0, y);
    lv_label_set_text(l, "");
    return l;
}

// The canvas buffer goes after the delete that is running has finished with the canvas (its destructor drops the image
// cache entry for it)
static void free_later(void *arg) {
    station_view_t *v = arg;
    lichen_free(NULL, v->dial_buf);
    free(v);
}

// The dial is going (its screen is deleted): the timer first, then the buffer and the view
static void on_dial_delete(lv_event_t *e) {
    station_view_t *v = lv_event_get_user_data(e);
    lv_timer_delete(v->timer);
    v->timer = NULL;
    lv_async_call(free_later, v);
}

// A screen covering the app's (the saver) stops the sweep; showing it again redraws the whole screen
static void on_screen_hidden(lv_event_t *e) {
    station_view_t *v = lv_event_get_user_data(e);
    lv_timer_pause(v->timer);
}

static void on_screen_shown(lv_event_t *e) {
    station_view_t *v = lv_event_get_user_data(e);
    lv_timer_resume(v->timer);
}

static void log_no_psram(int32_t d) {
    ESP_LOGW(TAG, "no PSRAM for the station clock's dial (%u bytes; %u free, largest block %u)",
             (unsigned)(lv_draw_buf_width_to_stride(d, LV_COLOR_FORMAT_RGB565) * d),
             (unsigned)lichen_sys_mem_free(LICHEN_SYS_MEM_PSRAM),
             (unsigned)lichen_sys_mem_largest(LICHEN_SYS_MEM_PSRAM));
}

station_view_t *station_view_create(lv_obj_t *parent, station_view_cfg_t const *cfg) {
    if (cfg->layout.dial_d < SC_MIN_DIAL) {
        return NULL;
    }
    station_view_t *v = calloc(1, sizeof(*v));
    if (!v) {
        return NULL;
    }
    v->d            = cfg->layout.dial_d;
    v->r            = v->d / 2;
    v->dims         = sc_dims(v->r);
    v->update_line2 = cfg->update_line2;
    if (!dial_create(v, parent, &cfg->layout)) {
        log_no_psram(v->d);
        free(v);
        return NULL;
    }
    v->date_label = add_line(parent, LICHEN_FONT_LARGE, THEME_DIM, cfg->layout.line1_y);
    v->line2      = add_line(parent, cfg->line2_font, cfg->line2_color, cfg->layout.line2_y);
    lv_obj_update_layout(parent);
    lv_area_t c;
    lv_obj_get_coords(v->dial, &c);
    v->cx = c.x1 + v->r;
    v->cy = c.y1 + v->r;
    lv_obj_add_event_cb(v->dial, on_draw, LV_EVENT_DRAW_MAIN_END, v);
    lv_obj_add_event_cb(v->dial, on_dial_delete, LV_EVENT_DELETE, v);
    frame(v); // the first frame now, not in 66 ms
    v->timer = lv_timer_create(tick, FRAME_MS, v);
    if (cfg->pause_when_hidden) {
        lv_obj_add_event_cb(parent, on_screen_hidden, LV_EVENT_SCREEN_UNLOADED, v);
        lv_obj_add_event_cb(parent, on_screen_shown, LV_EVENT_SCREEN_LOADED, v);
    }
    return v;
}

lv_obj_t *station_view_line2(station_view_t const *v) {
    return v->line2;
}

// lichen_ui.h's station clock for an app (#180): the Clock app's layout, under the status bar, a small dim second line
bool lichen_station_clock_impl(lv_obj_t *screen, void (*update_line2)(lv_obj_t *line2)) {
    lv_obj_update_layout(screen);
    int32_t line1_h = lv_font_get_line_height(lichen_font(LICHEN_FONT_LARGE));
    int32_t line2_h = lv_font_get_line_height(lichen_font(LICHEN_FONT_SMALL));
    station_view_cfg_t cfg = {
        .layout = sc_layout(lv_obj_get_content_width(screen), lv_obj_get_content_height(screen), line1_h, line2_h),
        .line2_font        = LICHEN_FONT_SMALL,
        .line2_color       = THEME_DIM,
        .update_line2      = update_line2,
        .pause_when_hidden = true,
    };
    return station_view_create(screen, &cfg) != NULL;
}

bool lichen_date_text_impl(char *out, size_t cap) {
    return saver_date_text(out, cap);
}
