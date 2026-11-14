// Settings > Wi-Fi > Wi-Fi analyser (#159): access points per 2.4 GHz channel as bars, and a signal history for one
// chosen network to walk towards it. One scan every LICHEN_SYS_WIFI_AN_SCAN_GAP_MS at most (scans disturb the link,
// #146); wifi_scan itself refuses during a firmware update. The scan runs on its own task, never on the LVGL task.
// Network names are shown here only: nothing is logged.
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_mem.h"
#include "lichen_sys_app.h"
#include "lichen_sys_net.h"
#include "lichen_sys_ui.h"
#include "lichen_task.h"
#include "lichen_ui.h"
#include "screen_ids.h"
#include "shell_parts.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>

#include <string.h>

#define MAX_RAW   32
#define TICK_MS   1000
#define GRAPH_W   648 // 720 - 2 * THEME_PAD
#define COL_W     (GRAPH_W / LICHEN_SYS_WIFI_AN_CHANNELS)
#define BAR_AREA  130
#define METER_H   90
#define METER_BAR 8
#define METER_GAP 2

static char const TAG[] = "wifi-an";

typedef struct {
    lichen_sys_wifi_ap_t    raw[MAX_RAW];  // the last scan, every AP
    lichen_sys_wifi_ap_t    list[MAX_RAW]; // the same, one per name, strongest first: what Up/Down choose from
    size_t            raw_n, list_n;
    lichen_sys_wifi_meter_t meter;
    char              watched[33];    // the network being followed ("" = the strongest one)
    bool              scanned;        // at least one scan came back
    bool              scanning;
    bool              ever;           // a scan was started
    uint32_t          last_ms;        // when the last scan started
    lichen_sys_wifi_ap_t    last_seen;      // the watched AP in the last scan
    bool              seen;
    lv_obj_t               *count[LICHEN_SYS_WIFI_AN_CHANNELS + 1];
    lv_obj_t               *bar[LICHEN_SYS_WIFI_AN_CHANNELS + 1];
    lv_obj_t               *meter_bar[LICHEN_SYS_WIFI_METER_LEN];
    lv_obj_t         *status, *name, *value;
    lv_timer_t       *timer;
} an_t;

static an_t    *an;      // PSRAM, while the screen is open
static uint32_t gen;     // a scan that reports after the screen was closed is dropped

typedef struct {
    lichen_sys_wifi_ap_t aps[MAX_RAW];
    size_t    n;
    bool      ok;
    uint32_t  gen;
} scan_res_t;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ---- drawing -----------------------------------------------------------------------------------------------------

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t colour) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(colour), 0);
    lv_obj_set_style_radius(o, 2, 0);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static lv_obj_t *small_label(lv_obj_t *parent, int x, int y, int w, char const *text, uint32_t colour) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_label_set_text(l, text);
    return l;
}

// 13 columns: the number of networks above the bar, the channel under it; the bar is as tall as the strongest signal
static lv_obj_t *build_graph(lv_obj_t *screen) {
    lv_obj_t *g = lv_obj_create(screen);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, GRAPH_W, 24 + BAR_AREA + 28);
    lv_obj_set_scrollable(g, false);
    lv_obj_set_clickable(g, false);
    for (int c = 1; c <= LICHEN_SYS_WIFI_AN_CHANNELS; c++) {
        int  x = (c - 1) * COL_W;
        char num[4];
        snprintf(num, sizeof(num), "%d", c);
        an->count[c] = small_label(g, x, 0, COL_W, "", THEME_TEXT);
        an->bar[c]   = box(g, x + 8, 24 + BAR_AREA - 2, COL_W - 16, 2, THEME_AMBER);
        small_label(g, x, 24 + BAR_AREA + 4, COL_W, num, THEME_DIM);
    }
    return g;
}

static lv_obj_t *build_meter(lv_obj_t *screen) {
    lv_obj_t *m = lv_obj_create(screen);
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, GRAPH_W, METER_H);
    lv_obj_set_scrollable(m, false);
    lv_obj_set_clickable(m, false);
    for (int i = 0; i < LICHEN_SYS_WIFI_METER_LEN; i++) {
        an->meter_bar[i] = box(m, i * (METER_BAR + METER_GAP), METER_H - 2, METER_BAR, 2, THEME_FAINT);
    }
    return m;
}

static void draw_channels(void) {
    lichen_sys_wifi_chan_stats_t st;
    lichen_sys_wifi_chan_stats(an->raw, an->raw_n, &st);
    for (int c = 1; c <= LICHEN_SYS_WIFI_AN_CHANNELS; c++) {
        char text[8] = "";
        if (an->scanned && st.count[c]) {
            snprintf(text, sizeof(text), "%u", st.count[c]);
        }
        lv_label_set_text(an->count[c], text);
        int h = an->scanned && st.count[c] ? lichen_sys_wifi_level_pct(st.best[c]) * BAR_AREA / 100 : 0;
        h     = h < 2 ? 2 : h;
        lv_obj_set_size(an->bar[c], COL_W - 16, h);
        lv_obj_set_y(an->bar[c], 24 + BAR_AREA - h);
    }
}

static void draw_meter(void) {
    for (int i = 0; i < LICHEN_SYS_WIFI_METER_LEN; i++) {
        // The newest reading is on the right; the older ones are to its left
        size_t age = LICHEN_SYS_WIFI_METER_LEN - 1 - (size_t)i;
        int8_t r =
            age < an->meter.n ? lichen_sys_wifi_meter_at(&an->meter, an->meter.n - 1 - age) : LICHEN_SYS_WIFI_RSSI_NONE;
        int h       = r == LICHEN_SYS_WIFI_RSSI_NONE ? 2 : lichen_sys_wifi_level_pct(r) * METER_H / 100;
        h           = h < 2 ? 2 : h;
        lv_obj_set_size(an->meter_bar[i], METER_BAR, h);
        lv_obj_set_y(an->meter_bar[i], METER_H - h);
        lv_obj_set_style_bg_color(
            an->meter_bar[i],
            lv_color_hex(r == LICHEN_SYS_WIFI_RSSI_NONE ? THEME_FAINT : THEME_TEAL),
            0
        );
    }
}

static void draw_watched(void) {
    char text[140];
    if (!an->scanned || an->list_n == 0) {
        lv_label_set_text(an->name, an->scanned ? "No network in range" : "Waiting for the first scan");
        lv_label_set_text(an->value, "");
        return;
    }
    char shown[140];
    lichen_sys_wifi_ssid_sanitize(an->watched, shown, sizeof(shown));
    lv_label_set_text(an->name, shown);
    if (an->seen) {
        snprintf(
            text,
            sizeof(text),
            "%d dBm, %d of 4 bars, channel %u",
            an->last_seen.rssi,
            lichen_sys_wifi_bars(an->last_seen.rssi),
            an->last_seen.channel
        );
    } else {
        snprintf(text, sizeof(text), "Not seen in the last scan");
    }
    lv_label_set_text(an->value, text);
}

static void set_status(char const *text) {
    lv_label_set_text(an->status, text);
}

// ---- scanning ----------------------------------------------------------------------------------------------------

// Rebuilds the choosable list (one per name, strongest first) and reads the watched network from the last scan
static void take_scan(scan_res_t const *res) {
    an->raw_n = res->ok ? res->n : 0;
    memcpy(an->raw, res->aps, an->raw_n * sizeof(an->raw[0]));
    memcpy(an->list, res->aps, an->raw_n * sizeof(an->list[0]));
    an->list_n = lichen_sys_wifi_list_clean(an->list, an->raw_n);
    if (an->watched[0] == '\0' && an->list_n) {
        snprintf(an->watched, sizeof(an->watched), "%s", an->list[0].ssid); // first look: the strongest network
    }
    an->seen = an->watched[0] && lichen_sys_wifi_an_lookup(an->raw, an->raw_n, an->watched, &an->last_seen);
    lichen_sys_wifi_meter_push(&an->meter, an->seen ? an->last_seen.rssi : LICHEN_SYS_WIFI_RSSI_NONE);
}

static void on_scan_done(void *data) {
    scan_res_t *res = data;
    if (!an || res->gen != gen) { // closed while the scan ran
        lichen_free(lichen_sys_ctx_os(), res);
        return;
    }
    an->scanning = false;
    if (res->ok) {
        an->scanned = true;
        take_scan(res);
        draw_channels();
        draw_meter();
        draw_watched();
        set_status("Scans every 10 s");
    } else {
        // Not at the user's expense: the next try waits the same 10 s
        set_status("Can't scan now: Wi-Fi is off or a firmware update is running. Trying again.");
    }
    lichen_free(lichen_sys_ctx_os(), res);
}

static void scan_task(void *arg) {
    scan_res_t *res = lichen_alloc(lichen_sys_ctx_os(), sizeof(*res), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (res) {
        res->gen = (uint32_t)(uintptr_t)arg;
        res->ok  = lichen_sys_wifi_scan_raw(res->aps, MAX_RAW, &res->n);
        if (!lichen_sys_ui_call(on_scan_done, res, 0)) { // waits for the LVGL lock as long as it takes, as before #225
            lichen_free(lichen_sys_ctx_os(), res);                        // not queued: still ours
        }
    } else {
        lichen_sys_ui_lock(0); // no memory: the screen must not wait for a scan that will never report
        if (an && (uint32_t)(uintptr_t)arg == gen) {
            an->scanning = false;
            set_status("Out of memory. Trying again.");
        }
        lichen_sys_ui_unlock();
    }
}

static void start_scan(void) {
    an->scanning = true;
    an->ever     = true;
    an->last_ms  = now_ms();
    set_status("Scanning...");
    if (!lichen_task_start(lichen_ctx_self(), "wifian", scan_task, (void *)(uintptr_t)gen, 4096, LICHEN_PRIO_JOB, NULL)) {
        an->scanning = false;
        ESP_LOGW(TAG, "scan task did not start");
        set_status("Could not start the scan. Trying again.");
    }
}

static void on_tick(lv_timer_t *timer) {
    (void)timer;
    if (an->scanning || !lichen_sys_wifi_an_scan_due(an->ever, an->last_ms, now_ms())) {
        return;
    }
    start_scan();
}

// Up/Down: the next stronger / weaker network; its history starts over
static void choose(int step) {
    if (!an->list_n) {
        return;
    }
    int at = -1;
    for (size_t i = 0; i < an->list_n; i++) {
        if (strcmp(an->list[i].ssid, an->watched) == 0) {
            at = (int)i;
        }
    }
    at = (at + step + (int)an->list_n) % (int)an->list_n;
    snprintf(an->watched, sizeof(an->watched), "%s", an->list[at].ssid);
    lichen_sys_wifi_meter_clear(&an->meter);
    an->seen = lichen_sys_wifi_an_lookup(an->raw, an->raw_n, an->watched, &an->last_seen);
    lichen_sys_wifi_meter_push(&an->meter, an->seen ? an->last_seen.rssi : LICHEN_SYS_WIFI_RSSI_NONE);
    draw_meter();
    draw_watched();
}

static void on_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (!an) {
        return;
    }
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        choose(key == LV_KEY_DOWN ? 1 : -1);
    } else if (key == LV_KEY_ENTER) {
        if (an->scanning) {
            return;
        }
        if (lichen_sys_wifi_an_scan_due(an->ever, an->last_ms, now_ms())) {
            start_scan();
        } else {
            uint32_t wait = (LICHEN_SYS_WIFI_AN_SCAN_GAP_MS - (now_ms() - an->last_ms) + 999) / 1000;
            char     text[64];
            snprintf(text, sizeof(text), "Next scan in %u s: scans disturb the Wi-Fi link", (unsigned)wait);
            set_status(text);
        }
    }
}

// ---- the screen --------------------------------------------------------------------------------------------------

static void on_delete(lv_event_t *e) {
    (void)e;
    gen++; // a scan still running is dropped when it reports
    if (an) {
        if (an->timer) {
            lv_timer_delete(an->timer);
        }
        lichen_free(lichen_sys_ctx_os(), an);
        an = NULL;
    }
}

static lv_obj_t *build(void) {
    lv_obj_t *screen = shell_page_create();
    an               = lichen_alloc(lichen_sys_ctx_os(), sizeof(*an), LICHEN_MEM_BULK | LICHEN_MEM_ZERO); // history and scans: PSRAM
    if (!an) {
        return screen;
    }
    gen++;
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 10, 0);
    lichen_heading(screen, NULL, "Wi-Fi analyser");
    lv_obj_t *cap = lv_label_create(screen);
    lv_obj_set_style_text_color(cap, lv_color_hex(THEME_DIM), 0);
    lv_label_set_text(cap, "Number: networks on the channel. Bar: strongest signal.");
    build_graph(screen);
    lv_obj_t *watch = lv_label_create(screen);
    lv_obj_set_style_text_color(watch, lv_color_hex(THEME_DIM), 0);
    lv_label_set_text(watch, "Signal of");
    an->name = lichen_label_line(screen, LV_PCT(100));
    lv_obj_set_style_text_color(an->name, lv_color_hex(THEME_AMBER), 0);
    an->value = lichen_label_line(screen, LV_PCT(100));
    build_meter(screen);
    an->status = lichen_label_wrap(screen, LV_PCT(100));
    lv_obj_set_style_text_color(an->status, lv_color_hex(THEME_DIM), 0);
    shell_page_hint(screen, "Up/Down", "other network", "Enter", "scan now", NULL);
    lichen_ui_event_add(screen, on_key, LV_EVENT_KEY, NULL);
    lichen_ui_event_add(screen, on_delete, LV_EVENT_DELETE, NULL);
    draw_channels();
    draw_meter();
    draw_watched();
    an->timer = lichen_ui_timer_create(on_tick, TICK_MS, NULL);
    on_tick(an->timer); // the first scan at once
    return screen;
}

void wifi_analyser_open(void) {
    shell_open(build, SCREEN_WIFI_ANALYSER);
}
