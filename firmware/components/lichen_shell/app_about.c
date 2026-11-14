// About Lichen (#66, lichen.html bottom): logo, what a lichen is, then the OS and the hardware only (the OS speaks only for
// itself; an app's own details are on its Settings page): version, free memory, the C6 link, Wi-Fi, the LoRa radio,
// storage, the SD card, uptime and the local clock. Up/Down scroll when it doesn't fit; the detail lines refresh once a second.
#include "about_dev.h"
#include "app_icons.h"
#include "dev_tap.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fonts.h"
#include "fw_console.h"
#include "lichen_app.h"
#include "lichen_reg.h"
#include "lichen_sys_c6.h"
#include "lichen_sys_clock.h"
#include "lichen_sys_keys.h"
#include "lichen_sys_mem.h"
#include "lichen_sys_notify.h"
#include "lichen_sys_net.h"
#include "lichen_sys_storage.h"
#include "lichen_ui.h"
#include "logo.h"
#include "os_name.h"
#include "shell_parts.h"
#include "theme.h"

#include <inttypes.h>
#include <stdio.h>

#include <string.h>
#include <time.h>

#define SCROLL_STEP      96

enum { ROW_WIFI, ROW_LORA, ROW_STORAGE, ROW_SD, ROW_UP, ROW_CLOCK, ROW_MEMORY, ROW_C6, ROW_DEV, ROWS }; // ROW_MEMORY on: for developers

// The registry keys under "For developers" (#334): the same cached getters as GET /info (ota_hw.c), through the gate's lichen_reg_get (#331); ROW_DEV + i is key i
static struct {
    char const *label;
    char const *key;
} const DEV[] = {
    {"PSRAM clock", "psram.clk"},          {"Display refresh", "disp.refresh"},
    {"Wi-Fi country", "wifi.country"},     {"LoRa power (dBm)", "lora.power"},
    {"Bluetooth", "bt.state"},             {"Write failures", "settings.write_fail"},
    {"Last write error", "settings.last_err"}, {"C6 registry caps", "c6.reg_caps"},
};
#define DEV_N (sizeof(DEV) / sizeof(DEV[0]))

static lv_obj_t *value_label[ROW_DEV + DEV_N];

// Developer mode (#374): the "For developers" block and the "Developer mode: on" row exist always and show only while
// the mode is on, so a console `fw dev on` or off reaches an open page within a second. Seven Enter presses switch it
// on (dev_tap.c); while on, Enter asks to switch it off.
enum { DEVOBJ_MODE, DEVOBJ_HEAD, DEVOBJ_FIRST_ROW, DEVOBJ_N = DEVOBJ_FIRST_ROW + ROW_DEV + DEV_N - ROW_MEMORY };
static lv_obj_t *dev_obj[DEVOBJ_N];
static lv_obj_t *page;
static dev_tap_t tap;
static int       dev_shown = -1; // what the page last showed: 0 off, 1 on, -1 not yet

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
    char name[48];
    snprintf(l1, c1, "%s", lichen_os_version_line(esp_app_get_description()->version, name, sizeof(name)));
    char up[24];
    fmt_uptime(up, sizeof(up));
    snprintf(l2, c2, "Uptime %s", up);
    return true;
}

static void set_text(lv_obj_t *label, char const *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void apply_dev(void) {
    int on = console_dev_mode() ? 1 : 0;
    if (on == dev_shown) {
        return;
    }
    dev_shown = on;
    for (size_t i = 0; i < DEVOBJ_N; i++) {
        lv_obj_set_hidden(dev_obj[i], !on);
    }
    if (on) {
        shell_page_hint(page, "Up/Down", "scroll", "Enter", "dev mode off", NULL);
    } else {
        shell_page_hint(page, "Up/Down", "scroll", NULL);
    }
}

static void refresh_net(void) {
    char text[96];
    char ip[16];
    if (lichen_sys_wifi_ip(ip, sizeof(ip))) {
        snprintf(text, sizeof(text), "connected, %s", ip);
    } else {
        strlcpy(text, "not connected", sizeof(text));
    }
    set_text(value_label[ROW_WIFI], text);
    set_text(value_label[ROW_LORA], lichen_sys_lora_up() ? "working" : "not available");
}

static void refresh_storage(void) {
    char     text[96];
    uint64_t free_b, total_b;
    if (lichen_sys_storage_free_bytes(&free_b, &total_b)) {
        snprintf(text, sizeof(text), "%.1f MB free of %.1f MB", (double)free_b / 1048576.0, (double)total_b / 1048576.0);
    } else {
        strlcpy(text, "not available", sizeof(text));
    }
    set_text(value_label[ROW_STORAGE], text);

    lichen_sd_info_t sd; // cached: the LVGL task never reads the card (#60)
    lichen_sys_sd_info(&sd);
    lichen_sys_sd_short_text(&sd, text, sizeof(text));
    set_text(value_label[ROW_SD], text);
}

static void refresh_clock(void) {
    char      text[96];
    time_t    now = time(NULL);
    struct tm local;
    if (lichen_sys_clock_is_set() && localtime_r(&now, &local)) {
        size_t n = strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
        char   src[48];
        lichen_sys_clock_source_text(src, sizeof(src)); // where the time came from (#83)
        snprintf(text + n, sizeof(text) - n, ", %s", src);
    } else {
        strlcpy(text, "not set", sizeof(text));
    }
    set_text(value_label[ROW_CLOCK], text);
}

static void refresh(lv_timer_t *timer) {
    (void)timer;
    char text[96];
    apply_dev();

    snprintf(text, sizeof(text), "%u KB internal " LV_SYMBOL_BULLET " %.1f MB PSRAM free",
             (unsigned)(lichen_sys_mem_free(LICHEN_SYS_MEM_INTERNAL) / 1024),
             (double)lichen_sys_mem_free(LICHEN_SYS_MEM_PSRAM) / 1048576.0);
    set_text(value_label[ROW_MEMORY], text);

    // While the C6 is being recovered, how far it has got (#14 T-B)
    if (!lichen_sys_c6_trouble(text, sizeof(text))) {
        strlcpy(text, lichen_sys_c6_up() ? "working" : "not answering", sizeof(text));
    }
    set_text(value_label[ROW_C6], text);

    refresh_net();
    refresh_storage();
    fmt_uptime(text, sizeof(text));
    set_text(value_label[ROW_UP], text);
    refresh_clock();
    for (size_t i = 0; i < DEV_N; i++) { // the cache only: never waits for the C6 or a bus (#307)
        char raw[96] = "";
        lichen_reg_get(DEV[i].key, raw, sizeof(raw));
        about_dev_text(DEV[i].key, raw, text, sizeof(text));
        set_text(value_label[ROW_DEV + i], text);
    }
}

// A release build has no developer mode (#373: its image holds none of the words, release_checks in build.sh), so
// Enter on About does nothing there: no taps, no dialog, no log line
#ifdef CONFIG_LICHEN_DEV_COMMANDS
static char const TAG[] = "about";

static void dev_off_answer(bool yes) {
    if (!yes) {
        return;
    }
    if (!console_dev_set(false)) {
        lichen_sys_notify(LN_WARN, "Developer mode off, not saved", "It comes back after a restart.", 0);
    }
    dev_tap_reset(&tap);
    if (page) {
        apply_dev();
    }
}

static void ask_dev_off(void) {
    shell_dialog_t const d = {
        .title         = "Switch developer mode off? The developer rows and commands go away.",
        .seconds       = 20,
        .careful       = true,
        .confirm_label = "switch off",
    };
    shell_dialog_countdown(&d, dev_off_answer);
}

// injected: read in on_key, on the press itself (#391: only valid synchronously in the key's LV_EVENT_KEY handler)
static void dev_enter(bool injected) {
    switch (dev_enter_action(&tap, console_dev_mode(), injected, lv_tick_get())) {
        case DEV_ENTER_REFUSED: ESP_LOGI(TAG, "injected Enter ignored: developer mode takes a hand or USB"); break;
        case DEV_ENTER_ASK_OFF: ask_dev_off(); break;
        case DEV_ENTER_SWITCH_ON: {
            bool saved = console_dev_set(true);
            lichen_sys_notify(LN_INFO, "Developer mode on", saved ? "Switch it off in About." : "This boot only: not saved.",
                              0);
            apply_dev();
            break;
        }
        default: break;
    }
}
#endif

static void on_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        shell_scroll_by(lv_event_get_target(e), key == LV_KEY_UP ? SCROLL_STEP : -SCROLL_STEP);
    }
#ifdef CONFIG_LICHEN_DEV_COMMANDS
    else if (key == LV_KEY_ENTER) {
        dev_enter(lichen_sys_keys_last_injected());
    }
#endif
}

static void on_delete(lv_event_t *e) {
    lv_timer_delete(lv_event_get_user_data(e));
    memset(value_label, 0, sizeof(value_label));
    memset(dev_obj, 0, sizeof(dev_obj));
    page      = NULL;
    dev_shown = -1;
}

static lv_obj_t *add_text(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lichen_label_wrap(parent, LV_PCT(100));
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
    static char const *const KEYS[] = {"Version", "Memory", "Radio chip", "Wi-Fi", "LoRa", "Internal storage", "SD card", "Uptime", "Clock", "Developer mode",
                                           "PSRAM clock", "Display refresh", "Wi-Fi country", "LoRa power (dBm)", "Bluetooth",
                                           "Write failures", "Last write error", "C6 registry caps"};
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
    lv_obj_set_style_text_letter_space(name, 3, 0);
    lv_label_set_text(name, lichen_os_name());
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

    add_row(screen, "Version", esp_app_get_description()->version);
    value_label[ROW_WIFI]    = add_row(screen, "Wi-Fi", NULL);
    value_label[ROW_LORA]    = add_row(screen, "LoRa", NULL);
    value_label[ROW_STORAGE] = add_row(screen, "Internal storage", NULL);
    value_label[ROW_SD]      = add_row(screen, "SD card", NULL);
    value_label[ROW_UP]      = add_row(screen, "Uptime", NULL);
    value_label[ROW_CLOCK]   = add_row(screen, "Clock", NULL);
    page = screen;
    dev_tap_reset(&tap);
    dev_shown = -1;
    dev_obj[DEVOBJ_MODE] = lv_obj_get_parent(add_row(screen, "Developer mode", "on"));
    dev_obj[DEVOBJ_HEAD] = add_text(screen, "For developers", THEME_DIM);
    value_label[ROW_MEMORY]  = add_row(screen, "Memory", NULL);
    value_label[ROW_C6]      = add_row(screen, "Radio chip", NULL);
    for (size_t i = 0; i < DEV_N; i++) {
        value_label[ROW_DEV + i] = add_row(screen, DEV[i].label, NULL);
    }
    for (size_t i = ROW_MEMORY; i < ROW_DEV + DEV_N; i++) {
        dev_obj[DEVOBJ_FIRST_ROW + i - ROW_MEMORY] = lv_obj_get_parent(value_label[i]);
    }
    // The last row ended flush on the pinned hint at the scroll end (#149): its own bottom pad is room that scrolls
    lv_obj_set_style_pad_bottom(lv_obj_get_parent(value_label[ROW_DEV + DEV_N - 1]), 12, 0);
    lv_obj_set_style_pad_bottom(lv_obj_get_parent(value_label[ROW_CLOCK]), 12, 0); // the same while the block is hidden

    lv_timer_t *timer = lv_timer_create(refresh, 1000, NULL);
    refresh(timer); // builds the hint too (apply_dev), after the padding above: it keeps the room it needs
    lv_obj_add_event_cb(screen, on_key, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, timer);
}

static lichen_app_t const about_app = {
    .name      = "About Lichen",
    .id        = "about",
    .draw_icon = icon_about,
    .tile_text = about_tile,
    .open      = open_about,
    .caps      = CAP_SYSTEM, // the shell's own app (#229)
};

void about_app_register(void) {
    lichen_register_app(&about_app);
}
