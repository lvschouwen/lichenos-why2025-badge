// The status bar over every screen after boot (#66, home.html), three columns (#104): logo and wordmark (and the C6
// trouble label) left, the clock centred (marked when unverified, #64), Wi-Fi and LoRa right. Wi-Fi fan (#58) and LoRa
// label are polled once a second from the LVGL task through shell_sources; the LoRa TX/RX chevrons from the packet
// counters every 100 ms (#29).
#include "lichen_clock.h"
#include "link_state.h"
#include "lora_activity.h"
#include "logo.h"
#include "shell_parts.h"
#include "fonts.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>
#include <time.h>


static lv_obj_t *clock_label;
static lv_obj_t *lora_label;
static lv_obj_t *wifi_label;
static lv_obj_t *c6_label; // only while the C6 is being recovered or gone (#14)
static int32_t   c6_max_w; // the room left of the clock (#104): longer C6 text ends in "..."

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

// The Wi-Fi fan (#58): a dot with three concentric 90-degree arcs above it, all on the dot's centre. 33 x 25 px,
// about the text height. The rounded caps of the outer arc end at x = 0 and 32, its top at y = 0, the dot at y = 24.
#define FAN_W         33
#define FAN_H         25
#define FAN_CX        16 // the dot's centre, from the box's top left
#define FAN_CY        22
#define FAN_DOT_R     3 // 6 px across, centred like lv_draw_arc's circles (cx - r .. cx + r - 1)
#define FAN_ARC_W     3  // thickness, inward from each outer radius
#define FAN_ARC_START 225 // LVGL's degrees run clockwise from 3 o'clock: 225-315 opens upwards
#define FAN_ARC_END   315

static int const fan_radius[WIFI_FAN_PARTS - 1] = {10, 16, 22}; // outer radii, inside out

static lv_obj_t *wifi_fan;
static int       fan_bars  = -1; // what the fan shows; -1 before the first look
static uint32_t  fan_color = THEME_FAINT;

static lv_color_t fan_part_color(int part) {
    return lv_color_hex(wifi_fan_part_lit(fan_bars, part) ? fan_color : THEME_FAINT);
}

static void fan_draw(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t   box;
    lv_obj_get_coords(lv_event_get_target_obj(e), &box);
    int32_t cx = box.x1 + FAN_CX, cy = box.y1 + FAN_CY;

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius      = LV_RADIUS_CIRCLE;
    dot.bg_color    = fan_part_color(0);
    lv_area_t area  = {cx - FAN_DOT_R, cy - FAN_DOT_R, cx + FAN_DOT_R - 1, cy + FAN_DOT_R - 1};
    lv_draw_rect(layer, &dot, &area);

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center.x    = cx;
    arc.center.y    = cy;
    arc.width       = FAN_ARC_W;
    arc.start_angle = FAN_ARC_START;
    arc.end_angle   = FAN_ARC_END;
    arc.rounded     = 1;
    for (int i = 0; i < WIFI_FAN_PARTS - 1; i++) {
        arc.radius = fan_radius[i];
        arc.color  = fan_part_color(i + 1);
        lv_draw_arc(layer, &arc);
    }
}

static lv_obj_t *wifi_fan_create(lv_obj_t *parent) {
    wifi_fan = lv_obj_create(parent);
    lv_obj_remove_style_all(wifi_fan);
    lv_obj_set_size(wifi_fan, FAN_W, FAN_H);
    lv_obj_set_scrollable(wifi_fan, false);
    lv_obj_set_clickable(wifi_fan, false);
    lv_obj_add_event_cb(wifi_fan, fan_draw, LV_EVENT_DRAW_MAIN, NULL);
    return wifi_fan;
}

// Lit parts in the look's colour up to its bars, the rest faint; redrawn only when the bars or the colour change
static void show_wifi(link_look_t look) {
    lv_obj_set_style_text_color(wifi_label, lv_color_hex(look.color), 0);
    if (look.bars == fan_bars && look.color == fan_color) {
        return;
    }
    fan_bars  = look.bars;
    fan_color = look.color;
    lv_obj_invalidate(wifi_fan);
}

static void show_lora(link_look_t look) {
    lv_obj_set_style_text_color(lora_label, lv_color_hex(look.color), 0);
}

typedef struct {
    lv_obj_t  *arrow;
    uint32_t   on_color; // TX amber (energy on air), RX teal (the radio half of the logo)
    lora_act_t act;
    uint8_t    shown; // the level its colour was last set for
} lora_chevron_t;

static lora_chevron_t tx_chevron = {.on_color = THEME_AMBER};
static lora_chevron_t rx_chevron = {.on_color = THEME_TEAL};

// Restyled only when the level moves: idle costs no redraw
static void step_chevron(lora_chevron_t *c, uint32_t count, uint32_t now_ms) {
    uint8_t level = lora_act_step(&c->act, count, now_ms);
    if (level == c->shown) {
        return;
    }
    c->shown = level;
    lv_color_t color = lv_color_mix(lv_color_hex(c->on_color), lv_color_hex(THEME_FAINT), level);
    lv_obj_set_style_text_color(c->arrow, color, 0);
}

// Every 100 ms: two loads under lora.c's state lock, nothing else unless a chevron's level changes. Comparing counts
// (they only go up) catches a packet whatever the poll rate, so nothing on the radio path has to flag it.
static void poll_lora_activity(lv_timer_t *timer) {
    (void)timer;
    uint32_t rx = 0, tx = 0;
    shell_sources.lora_counts(&rx, &tx);
    uint32_t now = lv_tick_get();
    step_chevron(&tx_chevron, tx, now);
    step_chevron(&rx_chevron, rx, now);
}

static void add_chevron(lv_obj_t *parent, lora_chevron_t *c, char const *symbol) {
    c->arrow = add_text(parent, symbol, THEME_FAINT);
    c->shown = 0; // faint, as created
}

// HH:MM for SNTP, by hand or a carried clock; ~HH:MM in amber for mesh time, unverified; --:-- dimmed when not set
// (#64). Only touched when the text or the colour changes.
static void show_clock(void) {
    static uint32_t shown_color;
    char            text[8] = "--:--";
    uint32_t        color   = THEME_DIM;
    time_t          now     = time(NULL);
    struct tm       local;
    if (clock_is_set() && localtime_r(&now, &local)) {
        bool approx = clock_is_approximate();
        strftime(text, sizeof(text), approx ? "~%H:%M" : "%H:%M", &local);
        color = approx ? THEME_AMBER : THEME_TEXT;
    }
    if (strcmp(lv_label_get_text(clock_label), text) != 0) {
        lv_label_set_text(clock_label, text);
    }
    if (color != shown_color) {
        lv_obj_set_style_text_color(clock_label, lv_color_hex(color), 0);
        shown_color = color;
    }
}

// As wide as the widest text it shows, right-aligned: the digits never move when a ~ comes or goes (#64), and the
// centred box keeps its place (#104)
static int32_t text_width(char const *text) {
    lv_point_t size;
    lv_text_get_size(&size, text, lichen_font(LICHEN_FONT_SMALL), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

static int32_t fix_clock_width(void) {
    int32_t w = text_width("~00:00");
    lv_obj_set_width(clock_label, w);
    lv_label_set_long_mode(clock_label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(clock_label, LV_TEXT_ALIGN_RIGHT, 0);
    return w;
}

// "C6: recovering 2/3" in amber, "C6: down (no answer)" in red, hidden while the C6 is up (#14 T-B)
static void show_c6(void) {
    char state[40], text[48];
    bool trouble = shell_sources.c6_trouble && shell_sources.c6_trouble(state, sizeof(state));
    if (!trouble) {
        lv_obj_set_hidden(c6_label, true);
        return;
    }
    snprintf(text, sizeof(text), "C6: %s", state);
    if (strcmp(lv_label_get_text(c6_label), text) != 0) {
        lv_obj_set_width(c6_label, LV_MIN(text_width(text), c6_max_w));
        lv_label_set_text(c6_label, text);
        bool down = strncmp(state, "down", 4) == 0;
        lv_obj_set_style_text_color(c6_label, lv_color_hex(down ? THEME_LINK_RED : THEME_AMBER), 0);
    }
    lv_obj_set_hidden(c6_label, false);
}

static void refresh(lv_timer_t *timer) {
    (void)timer;
    show_clock();
    show_c6();
    int8_t rssi  = 0;
    bool   known = shell_sources.wifi_rssi && shell_sources.wifi_rssi(&rssi);
    show_wifi(wifi_look(shell_sources.wifi_has_credentials && shell_sources.wifi_has_credentials(),
                        shell_sources.wifi_joining && shell_sources.wifi_joining(),
                        shell_sources.wifi_up && shell_sources.wifi_up(), known, rssi));
    show_lora(lora_look(shell_sources.lora_up && shell_sources.lora_up()));
}

#define BAR_PAD    24
#define LEFT_GAP   12
#define LOGO_PX    16
#define CLOCK_GAP  16 // the least room between the C6 text and the clock

// From the end of the wordmark to CLOCK_GAP before the centred clock: the C6 label's widest. Measured, so the clock
// and the right group never move however long the C6 state text gets (#104).
static int32_t c6_room(lv_obj_t *word, int32_t clock_w) {
    int32_t half  = lv_display_get_horizontal_resolution(lv_obj_get_display(word)) / 2;
    int32_t taken = BAR_PAD + LOGO_PX + LEFT_GAP + text_width(lv_label_get_text(word)) + LEFT_GAP;
    return LV_MAX(half - clock_w / 2 - CLOCK_GAP - taken, 0);
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
    lv_obj_set_style_pad_hor(bar, BAR_PAD, 0);
    lv_obj_set_style_text_font(bar, lichen_font(LICHEN_FONT_SMALL), 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_clickable(bar, false);

    lv_obj_t *left = add_row(bar, LEFT_GAP);
    logo_create(left, LOGO_PX);
    lv_obj_t *word = add_text(left, "lichen", THEME_DIM);
    c6_label       = add_text(left, "", THEME_AMBER);
    lv_label_set_long_mode(c6_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_hidden(c6_label, true);

    clock_label     = add_text(bar, "--:--", THEME_DIM);
    int32_t clock_w = fix_clock_width();
    lv_obj_add_flag(clock_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(clock_label, LV_ALIGN_CENTER, 0, 0);
    c6_max_w = c6_room(word, clock_w);

    // Right-aligned by the bar's SPACE_BETWEEN; about 50 px stay free to the clock for a notification dot (#62)
    lv_obj_t *right = add_row(bar, 28);
    lv_obj_t *wifi  = add_row(right, 6);
    wifi_label      = add_text(wifi, "Wi-Fi", THEME_DIM);
    wifi_fan_create(wifi);
    lv_obj_t *lora = add_row(right, 6);
    lora_label     = add_text(lora, "LoRa", THEME_DIM);
    if (shell_sources.lora_counts) {
        lv_obj_t *arrows = add_row(lora, 2); // the TX/RX chevrons (#29)
        add_chevron(arrows, &tx_chevron, LV_SYMBOL_UP);
        add_chevron(arrows, &rx_chevron, LV_SYMBOL_DOWN);
        poll_lora_activity(NULL); // primes both: the packets heard before the bar existed do not flash
        lv_timer_create(poll_lora_activity, 100, NULL);
    }

    refresh(NULL);
    lv_timer_create(refresh, 1000, NULL);
    return bar;
}
