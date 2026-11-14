// Clock (#122): the Swiss railway style station clock (lichen_station_clock, the screensaver's station_view.c, #119) as a home
// screen app: the dial sized to the content under the status bar, the date under it and, small, where the clock's time
// comes from and how old that is (lichen_clock). The idle rules still apply: the screensaver may start over it. Without
// PSRAM for the dial it shows the time as plain text. Only the OS API: nav and theme, time through lichen_clock.
#include "fonts.h"
#include "lichen_app.h"
#include "lichen_clock.h"
#include "lichen_ui.h"
#include "lichen_apps.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

// "SNTP, 3 min ago", "mesh, ..."; "approximate" when only an app has set it (unverified), "not set"
static void source_line(char *out, size_t cap) {
    char src[48];
    clock_source_text(src, sizeof(src));
    snprintf(out, cap, "%s%s", src, clock_is_approximate() ? " (approximate)" : "");
}

static void update_source(lv_obj_t *line) {
    char text[80];
    source_line(text, sizeof(text));
    lichen_label_set(line, text);
}

// The fallback when the dial cannot be built: HH:MM:SS and the date in one label, once a second
static void text_tick(lv_timer_t *t) {
    lv_obj_t *label = lv_timer_get_user_data(t);
    char      date[48], text[200];
    time_t    now = time(NULL);
    struct tm local;
    bool      ok = lichen_date_text(date, sizeof(date)) && localtime_r(&now, &local);
    if (ok) {
        char src[80];
        source_line(src, sizeof(src));
        snprintf(text, sizeof(text), "%02d:%02d:%02d\n%s\n%s", local.tm_hour, local.tm_min, local.tm_sec, date, src);
    } else {
        snprintf(text, sizeof(text), "%s", date);
    }
    lichen_label_set(label, text);
}

static void on_text_delete(lv_event_t *e) {
    lv_timer_delete(lv_event_get_user_data(e)); // before the label goes with its screen
}

static void open_text_clock(lv_obj_t *screen) {
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_style_text_font(label, lichen_font(LICHEN_FONT_LARGE), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_text(label, "");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    lv_timer_t *timer = lichen_ui_timer_create(text_tick, 1000, label);
    lichen_ui_event_add(screen, on_text_delete, LV_EVENT_DELETE, timer);
    text_tick(timer);
}

static void open_clock(lv_obj_t *screen) {
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    if (!lichen_station_clock(screen, update_source)) {
        open_text_clock(screen);
    }
}

static bool clock_tile(char *l1, size_t c1, char *l2, size_t c2) {
    time_t    now = time(NULL);
    struct tm local;
    if (!clock_is_set() || !localtime_r(&now, &local)) {
        return false;
    }
    snprintf(l1, c1, "%s%02d:%02d", clock_is_approximate() ? "~" : "", local.tm_hour, local.tm_min);
    snprintf(l2, c2, "station clock");
    return true;
}

// Clock (#122): a white station-clock face on the tile, the hour hand to 3, the minute hand to 12, a red centre
static void icon_clock(lv_obj_t *parent, int size_px) {
    lv_obj_set_style_radius(parent, size_px / 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    int face = size_px * 11 / 16, t = size_px >= 64 ? 4 : 2;
    lichen_icon_box(parent, face, face, 0, 0, 0xFFFFFF, LV_RADIUS_CIRCLE);
    lichen_icon_box(parent, t, face * 3 / 8 + t, 0, -face * 3 / 16, 0x111111, 0);       // minute hand, up
    lichen_icon_box(parent, face / 4 + t, t + t / 2, face / 8, 0, 0x111111, 0);          // hour hand, right
    lichen_icon_box(parent, t * 2, t * 2, 0, 0, 0xD8101C, LV_RADIUS_CIRCLE);            // the red centre
}

static lichen_app_t const clock_app = {
    .name             = "Clock",
    .id               = "clock",
    .draw_icon        = icon_clock,
    .tile_text        = clock_tile,
    .open             = open_clock,
    .caps             = 0,
    .internal_ram_kib = 1,
    .psram_kib        = 0,
};

void clock_app_register(void) {
    lichen_register_app(&clock_app);
}
