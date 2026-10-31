// About Lichen (#66, lichen.html bottom): logo, what a lichen is, then the OS and the hardware only (the OS speaks only for
// itself; an app's own details are on its Settings page): version, free memory, the C6 link, Wi-Fi, the LoRa radio,
// storage, uptime and the local clock. Up/Down scroll when it doesn't fit; the detail lines refresh once a second.
#include "app_icons.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lichen_clock.h"
#include "lichen_app.h"
#include "logo.h"
#include "shell_parts.h"
#include "storage.h"
#include "fonts.h"
#include "theme.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SCROLL_STEP      96

enum { ROW_MEMORY, ROW_C6, ROW_WIFI, ROW_LORA, ROW_STORAGE, ROW_UP, ROW_CLOCK, ROWS };

static lv_obj_t *value_label[ROWS];

static void fmt_uptime(char *out, size_t cap) {
    uint64_t s = (uint64_t)(esp_timer_get_time() / 1000000);
    if (s >= 86400) {
        snprintf(out, cap, "%" PRIu64 " d %" PRIu64 " h", s / 86400, s % 86400 / 3600);
    } else if (s >= 3600) {
        snprintf(out, cap, "%" PRIu64 " h %" PRIu64 " min", s / 3600, s % 3600 / 60);
    } else if (s >= 60) {
        snprintf(out, cap, "%" PRIu64 " min", s / 60);
    } else {
        snprintf(out, cap, "%" PRIu64 " s", s);
    }
}

static bool about_tile(char *l1, size_t c1, char *l2, size_t c2) {
    snprintf(l1, c1, "version %s", esp_app_get_description()->version);
    char up[24];
    fmt_uptime(up, sizeof(up));
    snprintf(l2, c2, "up %s", up);
    return true;
}

static void set_text(lv_obj_t *label, char const *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void refresh(lv_timer_t *timer) {
    (void)timer;
    char text[96];

    snprintf(text, sizeof(text), "%u KB  " LV_SYMBOL_BULLET "  PSRAM %.1f MB free",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
             (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0);
    set_text(value_label[ROW_MEMORY], text);

    set_text(value_label[ROW_C6], shell_sources.c6_up && shell_sources.c6_up() ? "up" : "down");

    char ip[16];
    if (shell_sources.wifi_ip && shell_sources.wifi_ip(ip, sizeof(ip))) {
        snprintf(text, sizeof(text), "connected, %s", ip);
    } else {
        strlcpy(text, "not connected", sizeof(text));
    }
    set_text(value_label[ROW_WIFI], text);

    set_text(value_label[ROW_LORA], shell_sources.lora_up && shell_sources.lora_up() ? "radio up" : "radio down");

    uint64_t free_b, total_b;
    if (storage_free_bytes(&free_b, &total_b)) {
        snprintf(text, sizeof(text), "%.1f / %.1f MB free", (double)free_b / 1048576.0, (double)total_b / 1048576.0);
    } else {
        strlcpy(text, "-", sizeof(text));
    }
    set_text(value_label[ROW_STORAGE], text);

    fmt_uptime(text, sizeof(text));
    set_text(value_label[ROW_UP], text);

    time_t    now = time(NULL);
    struct tm local;
    if (clock_is_set() && localtime_r(&now, &local)) {
        size_t n = strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
        char   src[48];
        clock_source_text(src, sizeof(src)); // where the time came from (#83)
        snprintf(text + n, sizeof(text) - n, ", %s", src);
    } else {
        strlcpy(text, "not set", sizeof(text));
    }
    set_text(value_label[ROW_CLOCK], text);
}

static void on_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        shell_scroll_by(lv_event_get_target(e), key == LV_KEY_UP ? SCROLL_STEP : -SCROLL_STEP);
    }
}

static void on_delete(lv_event_t *e) {
    lv_timer_delete(lv_event_get_user_data(e));
    memset(value_label, 0, sizeof(value_label));
}

static lv_obj_t *add_text(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

// Width of the key column: the widest key in the current font plus a gap, so a taller font cannot make a key touch
// its value (#28)
static int32_t key_col_w(lv_obj_t *obj, char const *const *keys, size_t n) {
    lv_font_t const *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    int32_t          w    = 0;
    for (size_t i = 0; i < n; i++) {
        lv_point_t size;
        lv_text_get_size(&size, keys[i], font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        w = size.x > w ? size.x : w;
    }
    return w + 24;
}

static lv_obj_t *add_row(lv_obj_t *parent, char const *key, char const *fixed) {
    static char const *const KEYS[] = {"version", "memory", "C6 link", "Wi-Fi", "LoRa", "storage", "up", "clock"};
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(row, false);
    lv_obj_t *k = lv_label_create(row);
    lv_obj_set_width(k, key_col_w(parent, KEYS, sizeof(KEYS) / sizeof(KEYS[0])));
    lv_obj_set_style_text_color(k, lv_color_hex(THEME_DIM), 0);
    lv_label_set_text(k, key);
    lv_obj_t *v = lv_label_create(row);
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_text(v, fixed ? fixed : "");
    return v;
}

static void open_about(lv_obj_t *screen) {
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 16, 0);
    lv_obj_set_style_pad_bottom(screen, THEME_PAD, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUS_KEY);

    lv_obj_t *head = lv_obj_create(screen);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 24, 0);
    lv_obj_set_scrollable(head, false);
    logo_create(head, 100);
    lv_obj_t *names = lv_obj_create(head);
    lv_obj_remove_style_all(names);
    lv_obj_set_size(names, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(names, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(names, 4, 0);
    lv_obj_t *name = lv_label_create(names);
    lv_obj_set_style_text_font(name, lichen_font(LICHEN_FONT_LARGE), 0);
    lv_obj_set_style_text_letter_space(name, 6, 0);
    lv_label_set_text(name, "lichen");
    lv_obj_t *sub = lv_label_create(names);
    lv_obj_set_style_text_color(sub, lv_color_hex(THEME_DIM), 0);
    lv_label_set_text(sub, "firmware for the WHY2025 badge");

    // Spec goal 6, short
    add_text(screen, "A lichen isn't one organism but two: a fungus and an alga living as one. Neither could live "
                     "there alone.", THEME_TEXT);
    add_text(screen, "This badge works the same way. The P4 does the thinking and the screen; the C6 talks to the "
                     "world over Wi-Fi, Bluetooth and LoRa. And like lichen, it spreads slowly from stone to stone: "
                     "one node, then a mesh.", THEME_TEXT);
    // GPL-3.0 section 0 "Appropriate Legal Notices"; the MIT/BSD notices are in THIRD_PARTY_LICENSES.txt (#18)
    add_text(screen, "Free software under the GPL-3.0; third-party code keeps its own license. Source and license "
                     "texts: github.com/lvschouwen/lichenos-why2025-badge", THEME_DIM);

    lv_obj_t *rule = lv_obj_create(screen);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, LV_PCT(100), 2);
    lv_obj_set_style_bg_color(rule, lv_color_hex(THEME_RULE), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);

    add_row(screen, "version", esp_app_get_description()->version);
    value_label[ROW_MEMORY]  = add_row(screen, "memory", NULL);
    value_label[ROW_C6]      = add_row(screen, "C6 link", NULL);
    value_label[ROW_WIFI]    = add_row(screen, "Wi-Fi", NULL);
    value_label[ROW_LORA]    = add_row(screen, "LoRa", NULL);
    value_label[ROW_STORAGE] = add_row(screen, "storage", NULL);
    value_label[ROW_UP]      = add_row(screen, "up", NULL);
    value_label[ROW_CLOCK]   = add_row(screen, "clock", NULL);

    lv_timer_t *timer = lv_timer_create(refresh, 1000, NULL);
    refresh(timer);
    lv_obj_add_event_cb(screen, on_key, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, timer);
}

static lichen_app_t const about_app = {
    .name      = "About Lichen",
    .id        = "about",
    .draw_icon = icon_about,
    .tile_text = about_tile,
    .open      = open_about,
};

void about_app_register(void) {
    lichen_register_app(&about_app);
}
