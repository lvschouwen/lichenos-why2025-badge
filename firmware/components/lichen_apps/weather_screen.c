// Weather (#155): the app's screen. Four rows with the readings now (temperature, humidity, pressure and the air
// quality as a trend), the chosen quantity drawn as a graph of the history below. Left/Right (or Enter) switch the
// quantity. The rows refresh every second; the graph only when a new minute sample or a new offset arrives.
#include "fonts.h"
#include "esp_attr.h"
#include "lichen_ui.h"
#include "theme.h"
#include "weather_app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define REFRESH_MS 1000
#define CHART_H    160 // 250 pushed the lines under the chart below the pinned hint (bench 2026-11-04)
#define TEXT_CAP   128 // high, low and span (31 each) plus the separators

typedef struct {
    lv_obj_t          *name[WX_QTY];
    lv_obj_t          *value[WX_QTY];
    lv_obj_t          *chart, *high_low, *note;
    lv_chart_series_t *ser;
    wx_qty_t           sel;
    unsigned           drawn_version;
    wx_qty_t           drawn_sel;
    bool               drawn;
    wx_air_t           air;
    float              series[WX_RING_CAP], gas[WX_RING_CAP]; // the chosen quantity; the air trend always needs the gas
} wx_ui_t;

EXT_RAM_BSS_ATTR static wx_ui_t s_ui; // PSRAM (#264): LVGL task only

static void set_text(lv_obj_t *label, char const *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void show_rows(void) {
    wx_sample_t now;
    bool        have = weather_current(&now);
    for (int q = 0; q < WX_QTY; q++) {
        char text[40];
        if (q == WX_GAS) {
            snprintf(text, sizeof(text), "%s", have && !isnan(now.v[q]) ? wx_air_text(s_ui.air) : "no reading");
        } else {
            wx_fmt_value(text, sizeof(text), (wx_qty_t)q, have ? now.v[q] : NAN);
        }
        set_text(s_ui.value[q], text);
        lv_color_t c = lv_color_hex(q == (int)s_ui.sel ? THEME_AMBER : THEME_TEXT);
        lv_obj_set_style_text_color(s_ui.name[q], c, 0);
        lv_obj_set_style_text_color(s_ui.value[q], c, 0);
    }
}

// The graph, the high/low line and the explanation under it for the chosen quantity
static void show_chart(void) {
    size_t n = weather_series(s_ui.sel, s_ui.series, WX_RING_CAP);
    size_t gn = weather_series(WX_GAS, s_ui.gas, WX_RING_CAP);
    s_ui.air  = wx_air_trend(s_ui.gas, gn);
    float lo, hi;
    char  a[32], b[32], text[TEXT_CAP];
    if (n < 2 || !wx_range(s_ui.series, n, s_ui.sel, &lo, &hi)) {
        lv_chart_set_point_count(s_ui.chart, 2);
        lv_chart_set_series_value_by_id(s_ui.chart, s_ui.ser, 0, LV_CHART_POINT_NONE);
        lv_chart_set_series_value_by_id(s_ui.chart, s_ui.ser, 1, LV_CHART_POINT_NONE);
        set_text(s_ui.high_low, "Collecting: one reading a minute.");
        return;
    }
    lv_chart_set_axis_range(s_ui.chart, LV_CHART_AXIS_PRIMARY_Y, wx_chart_value(floorf(lo)), wx_chart_value(ceilf(hi)));
    lv_chart_set_point_count(s_ui.chart, (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        lv_chart_set_series_value_by_id(s_ui.chart, s_ui.ser, (uint32_t)i,
                                        isnan(s_ui.series[i]) ? LV_CHART_POINT_NONE : wx_chart_value(s_ui.series[i]));
    }
    wx_minmax(s_ui.series, n, &lo, &hi);
    wx_fmt_value(a, sizeof(a), s_ui.sel, hi);
    wx_fmt_value(b, sizeof(b), s_ui.sel, lo);
    char span[32];
    wx_span_text(span, sizeof(span), n);
    // one line: a second line for the span pushed the gas note under the hint
    snprintf(text, sizeof(text), "High %s  \xE2\x80\xA2  Low %s  \xE2\x80\xA2  last %s", a, b, span);
    set_text(s_ui.high_low, text);
}

static void show_note(void) {
    set_text(s_ui.note, s_ui.sel == WX_GAS ? "Gas resistance in kOhm: higher is cleaner air. Compared with earlier "
                                              "in this room, not an absolute index."
                                            : "");
}

static void refresh(lv_timer_t *timer) {
    (void)timer;
    unsigned v = weather_version();
    if (!s_ui.drawn || v != s_ui.drawn_version || s_ui.sel != s_ui.drawn_sel) {
        show_chart();
        show_note();
        s_ui.drawn         = true;
        s_ui.drawn_version = v;
        s_ui.drawn_sel     = s_ui.sel;
    }
    show_rows();
}

static void on_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    int      d   = (key == LV_KEY_RIGHT || key == LV_KEY_ENTER) ? 1 : key == LV_KEY_LEFT ? WX_QTY - 1 : 0;
    if (d) {
        s_ui.sel = (wx_qty_t)((s_ui.sel + d) % WX_QTY);
        refresh(NULL);
    }
}

static void on_delete(lv_event_t *e) {
    lv_timer_delete(lv_event_get_user_data(e));
    memset(&s_ui, 0, sizeof(s_ui));
}

static void add_row(lv_obj_t *screen, int q) {
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_ui.name[q]  = lv_label_create(row);
    s_ui.value[q] = lv_label_create(row);
    lv_label_set_text(s_ui.name[q], wx_qty_name((wx_qty_t)q));
    lv_label_set_text(s_ui.value[q], "");
}

static void build_chart(lv_obj_t *screen) {
    lv_obj_t *c = lv_chart_create(screen);
    lv_obj_set_size(c, LV_PCT(100), CHART_H);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(c, 3, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(THEME_TILE), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_set_style_line_color(c, lv_color_hex(THEME_FAINT), LV_PART_MAIN);
    lv_obj_set_style_line_width(c, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(c, 0, 0, LV_PART_INDICATOR); // a line, no dot per minute
    s_ui.chart = c;
    s_ui.ser   = lv_chart_add_series(c, lv_color_hex(THEME_TEXT), LV_CHART_AXIS_PRIMARY_Y);
}

static lv_obj_t *dim_label(lv_obj_t *parent) {
    lv_obj_t *l = lichen_label_wrap(parent, LV_PCT(100));
    lv_obj_set_style_text_color(l, lv_color_hex(THEME_DIM), 0);
    lv_label_set_text(l, "");
    return l;
}

void weather_screen_open(lv_obj_t *screen) {
    memset(&s_ui, 0, sizeof(s_ui));
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 10, 0);
    lv_obj_set_style_pad_bottom(screen, THEME_PAD + 40, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lichen_heading(screen, NULL, "Weather");
    for (int q = 0; q < WX_QTY; q++) {
        add_row(screen, q);
    }
    build_chart(screen);
    s_ui.high_low = dim_label(screen);
    s_ui.note     = dim_label(screen);
    shell_page_hint(screen, "Left/Right", "quantity", NULL);
    lv_timer_t *timer = lichen_ui_timer_create(refresh, REFRESH_MS, NULL);
    lichen_ui_event_add(screen, on_key, LV_EVENT_KEY, NULL);
    lichen_ui_event_add(screen, on_delete, LV_EVENT_DELETE, timer);
    refresh(timer);
}

// A thermometer on the tile: a white tube over a red bulb
void icon_weather(lv_obj_t *parent, int size_px) {
    lv_obj_set_style_radius(parent, size_px / 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    int tube_w = size_px / 8 + 2, tube_h = size_px * 7 / 16, bulb = size_px / 3;
    lv_obj_t *tube = lv_obj_create(parent);
    lv_obj_remove_style_all(tube);
    lv_obj_set_size(tube, tube_w, tube_h);
    lv_obj_set_style_radius(tube, tube_w / 2, 0);
    lv_obj_set_style_bg_color(tube, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(tube, LV_OPA_COVER, 0);
    lv_obj_align(tube, LV_ALIGN_CENTER, 0, -size_px / 8);
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, bulb, bulb);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(THEME_RED), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, size_px / 4);
}
