// The status bar over every screen after boot (#66, home.html): logo, clock, Wi-Fi, LoRa. Wi-Fi bars and LoRa label, polled once a second from
// the LVGL task through shell_sources.
#include "link_state.h"
#include "logo.h"
#include "shell_parts.h"
#include "theme.h"

#include <string.h>
#include <time.h>

#define CLOCK_VALID_FROM 1767225600 // 2026-01-01: anything earlier means SNTP hasn't answered yet

static lv_obj_t *clock_label;
static lv_obj_t *lora_label;
static lv_obj_t *wifi_label;

static lv_obj_t *add_row(lv_obj_t *parent, int gap) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, gap, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, false);
    return row;
}

static lv_obj_t *add_text(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

#define BAR_COUNT 4
#define BAR_W     3
#define BAR_GAP   2

static lv_obj_t *wifi_bars_obj[BAR_COUNT];

// Four bars of 25/50/75/100 % of the text height: filled in the look's colour up to `bars`, the rest as outlines in
// the same colour, so a red empty set reads as "down" without words
static lv_obj_t *wifi_bars_create(lv_obj_t *parent) {
    int       h    = lv_font_get_line_height(&lv_font_montserrat_24);
    lv_obj_t *box  = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, BAR_COUNT * BAR_W + (BAR_COUNT - 1) * BAR_GAP, h);
    lv_obj_set_scrollable(box, false);
    lv_obj_set_clickable(box, false);
    for (int i = 0; i < BAR_COUNT; i++) {
        int       bh  = h * (i + 1) / BAR_COUNT;
        lv_obj_t *bar = lv_obj_create(box);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, BAR_W, bh);
        lv_obj_set_pos(bar, i * (BAR_W + BAR_GAP), h - bh);
        lv_obj_set_style_border_width(bar, 1, 0);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_set_clickable(bar, false);
        wifi_bars_obj[i] = bar;
    }
    return box;
}

static void show_wifi(link_look_t look) {
    lv_obj_set_style_text_color(wifi_label, lv_color_hex(look.color), 0);
    for (int i = 0; i < BAR_COUNT; i++) {
        lv_color_t c = lv_color_hex(look.color);
        lv_obj_set_style_border_color(wifi_bars_obj[i], c, 0);
        lv_obj_set_style_bg_color(wifi_bars_obj[i], c, 0);
        lv_obj_set_style_bg_opa(wifi_bars_obj[i], i < look.bars ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

static void show_lora(link_look_t look) {
    lv_obj_set_style_text_color(lora_label, lv_color_hex(look.color), 0);
}

static void refresh(lv_timer_t *timer) {
    (void)timer;
    char      text[8] = "--:--";
    time_t    now     = time(NULL);
    struct tm local;
    if (now >= (time_t)CLOCK_VALID_FROM && localtime_r(&now, &local)) {
        strftime(text, sizeof(text), "%H:%M", &local);
    }
    if (strcmp(lv_label_get_text(clock_label), text) != 0) {
        lv_label_set_text(clock_label, text);
    }
    int8_t rssi  = 0;
    bool   known = shell_sources.wifi_rssi && shell_sources.wifi_rssi(&rssi);
    show_wifi(wifi_look(shell_sources.wifi_has_credentials && shell_sources.wifi_has_credentials(),
                        shell_sources.wifi_joining && shell_sources.wifi_joining(),
                        shell_sources.wifi_up && shell_sources.wifi_up(), known, rssi));
    show_lora(lora_look(shell_sources.lora_up && shell_sources.lora_up()));
}

lv_obj_t *status_bar_create(lv_obj_t *parent) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), THEME_STATUS_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(THEME_RULE), 0);
    lv_obj_set_style_pad_hor(bar, 24, 0);
    lv_obj_set_style_text_font(bar, &lv_font_montserrat_24, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_clickable(bar, false);

    lv_obj_t *left = add_row(bar, 12);
    logo_create(left, 16);
    add_text(left, "lichen", THEME_DIM);

    lv_obj_t *right = add_row(bar, 28);
    clock_label     = add_text(right, "--:--", THEME_TEXT);
    lv_obj_t *wifi = add_row(right, 6);
    wifi_label     = add_text(wifi, "Wi-Fi", THEME_DIM);
    wifi_bars_create(wifi);
    lora_label = add_text(right, "LoRa", THEME_DIM);

    refresh(NULL);
    lv_timer_create(refresh, 1000, NULL);
    return bar;
}
