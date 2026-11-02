// The station clock screensaver (#119): a Swiss railway style dial, white with black bars, black hands and a red
// stop-to-go second hand (station_clock.c has the times and the geometry). The 720x720 panel is flushed by the CPU and
// turned in software, so the sweep must not redraw the screen: the dial is drawn once into a canvas in PSRAM (an
// image blit per redraw, no 60 anti-aliased bars), the hands are drawn on top of it in the canvas's draw event, and a
// ~15 fps timer invalidates only the second hand's old and new places, split along the hand so a diagonal hand is a
// few small boxes instead of one big one. The minute and hour hands are invalidated once a minute, when they move.
// No drift (unlike the seven-segment clock): the dial fills the screen and the panel is an LCD.
// Everything here runs on the LVGL task (the timer and the saver's build in screen_idle's tick).
#include "saver_station.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fonts.h"
#include "lichen_clock.h"
#include "station_clock.h"
#include "theme.h"

#include <stdint.h>

#include <string.h>
#include <sys/time.h>
#include <time.h>

static char const TAG[] = "saver";

#define FRAME_MS      66  // ~15 fps: the tip of the second hand moves ~2 px a frame
#define DIAL_D        576 // the dial's diameter; the date and the unread count go under it
#define DIAL_R        (DIAL_D / 2)
#define DIAL_TOP      24
#define DATE_GAP      8
#define PAD           2 // the anti-aliased edge around a redrawn hand
#define SECOND_PIECES 4
#define HAND_PIECES   2

#define DIAL_WHITE 0xFFFFFF
#define HAND_BLACK 0x111111
#define SIGNAL_RED 0xD8101C

typedef struct {
    bool    set; // no hands while the clock is not set
    int32_t hour, minute, second;
} hands_t;

static lv_obj_t   *dial;
static uint8_t    *dial_buf;
static lv_obj_t   *date_label, *notes_label;
static lv_timer_t *timer;
static sc_dims_t   dims;
static int32_t     cx, cy; // the dial's centre, screen coordinates
static hands_t     shown;  // what the dial shows (the draw event draws this, the timer moves it)
static time_t      labels_at;

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
static bool dial_create(lv_obj_t *screen) {
    uint32_t stride = lv_draw_buf_width_to_stride(DIAL_D, LV_COLOR_FORMAT_RGB565);
    dial_buf        = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, (size_t)stride * DIAL_D, MALLOC_CAP_SPIRAM);
    if (!dial_buf) {
        return false;
    }
    dial = lv_canvas_create(screen);
    lv_canvas_set_buffer(dial, dial_buf, DIAL_D, DIAL_D, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(dial, lv_color_black(), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(dial, &layer);
    lv_draw_rect_dsc_t face;
    lv_draw_rect_dsc_init(&face);
    face.radius   = LV_RADIUS_CIRCLE;
    face.bg_color = lv_color_hex(DIAL_WHITE);
    lv_area_t all = {0, 0, DIAL_D - 1, DIAL_D - 1};
    lv_draw_rect(&layer, &face, &all);
    for (int i = 0; i < 60; i++) {
        draw_bar(&layer, DIAL_R, DIAL_R, i * 60, i % 5 ? dims.minute_mark : dims.hour_mark, HAND_BLACK);
    }
    lv_canvas_finish_layer(dial, &layer);
    lv_obj_align(dial, LV_ALIGN_TOP_MID, 0, DIAL_TOP);
    return true;
}

static void on_draw(lv_event_t *e) {
    if (!shown.set) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    draw_bar(layer, cx, cy, shown.hour, dims.hour, HAND_BLACK);
    draw_bar(layer, cx, cy, shown.minute, dims.minute, HAND_BLACK);
    draw_bar(layer, cx, cy, shown.second, dims.second, SIGNAL_RED);
    draw_disc(layer, sc_polar(cx, cy, shown.second, dims.second.r1), dims.disc_r, SIGNAL_RED);
    draw_disc(layer, (sc_point_t){cx, cy}, dims.cap_r, SIGNAL_RED);
}

static void invalidate_box(sc_box_t b) {
    lv_area_t a = {b.x1, b.y1, b.x2, b.y2};
    lv_obj_invalidate_area(dial, &a);
}

static void invalidate_bar(int32_t a, sc_bar_t bar, int32_t pieces) {
    int32_t len = bar.r1 - bar.r0;
    for (int32_t k = 0; k < pieces; k++) {
        invalidate_box(sc_bar_box(cx, cy, a, bar.w, bar.r0 + len * k / pieces, bar.r0 + len * (k + 1) / pieces, PAD));
    }
}

static void invalidate_second(int32_t a) {
    invalidate_bar(a, dims.second, SECOND_PIECES);
    invalidate_box(sc_disc_box(cx, cy, a, dims.second.r1, dims.disc_r, PAD));
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

static void labels_update(void) {
    char date[48];
    saver_date_text(date, sizeof(date));
    if (strcmp(lv_label_get_text(date_label), date) != 0) {
        lv_label_set_text(date_label, date);
    }
    saver_notes_update(notes_label);
}

static void tick(lv_timer_t *t) {
    (void)t;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    hands_t now = hands_now(&tv);
    if (now.set != shown.set) {
        lv_obj_invalidate(dial); // the hands appear or go: once
    } else if (now.set) {
        if (now.minute != shown.minute || now.hour != shown.hour) { // the jump on the minute
            invalidate_bar(shown.hour, dims.hour, HAND_PIECES);
            invalidate_bar(now.hour, dims.hour, HAND_PIECES);
            invalidate_bar(shown.minute, dims.minute, HAND_PIECES);
            invalidate_bar(now.minute, dims.minute, HAND_PIECES);
        }
        if (now.second != shown.second) { // nothing during the pause at 12
            invalidate_second(shown.second);
            invalidate_second(now.second);
        }
    }
    shown = now;
    if (tv.tv_sec != labels_at) {
        labels_at = tv.tv_sec;
        labels_update();
    }
}

static lv_obj_t *add_line(lv_obj_t *screen, int font, uint32_t color, int32_t y) {
    lv_obj_t *l = lv_label_create(screen);
    lv_obj_set_style_text_font(l, lichen_font(font), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, LV_PCT(100)); // centred whatever the text: the dial never moves
    lv_obj_set_pos(l, 0, y);
    lv_label_set_text(l, "");
    return l;
}

bool saver_station_create(lv_obj_t *screen) {
    dims = sc_dims(DIAL_R);
    if (!dial_create(screen)) {
        ESP_LOGW(TAG, "no PSRAM for the station clock's dial: the digital clock instead");
        return false;
    }
    int32_t line_h = lv_font_get_line_height(lichen_font(LICHEN_FONT_LARGE));
    date_label     = add_line(screen, LICHEN_FONT_LARGE, THEME_DIM, DIAL_TOP + DIAL_D + DATE_GAP);
    notes_label    = add_line(screen, LICHEN_FONT_SMALL, THEME_AMBER, DIAL_TOP + DIAL_D + DATE_GAP + line_h);
    lv_obj_update_layout(screen);
    lv_area_t c;
    lv_obj_get_coords(dial, &c);
    cx = c.x1 + DIAL_R;
    cy = c.y1 + DIAL_R;
    lv_obj_add_event_cb(dial, on_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    shown     = (hands_t){0};
    labels_at = 0;
    tick(NULL);
    timer = lv_timer_create(tick, FRAME_MS, NULL);
    return true;
}

void saver_station_delete(void) {
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    dial        = NULL;
    date_label  = NULL;
    notes_label = NULL;
    heap_caps_free(dial_buf);
    dial_buf = NULL;
}
