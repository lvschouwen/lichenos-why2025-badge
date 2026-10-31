// Settings > Wi-Fi (#66): scan on core 0, pick a network, type the password, join. The scan blocks for seconds over
// ESP-Hosted, so it never runs on the LVGL task; its result comes back through lv_async_call and is dropped if the
// screen is gone by then.
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "menu.h"
#include "settings_store.h"
#include "shell_parts.h"
#include "text_entry.h"
#include "theme.h"
#include "wifi_list.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_WIFI     100
#define MAX_APS         20
#define POLL_MS         500

static char const TAG[] = "wifi";

typedef struct {
    wifi_ap_t aps[MAX_APS];
    size_t    n;
    bool      ok;
    uint32_t  gen; // the screen build this scan belongs to
} scan_result_t;

static lv_obj_t   *screen;
static lv_obj_t   *menu;
static lv_obj_t   *status;
static lv_group_t *group;
static lv_timer_t *poll;
static uint32_t    gen;
static lv_obj_t   *rows[MAX_APS + 2]; // the placeholder or the networks, then "Scan again"
static size_t      row_count;
static wifi_ap_t   shown[MAX_APS];
static size_t      shown_n;
static char        target[33];
static void      (*setup_done)(void); // set while this is the first-run Wi-Fi step

__attribute__((format(printf, 1, 2))) static void set_status(char const *fmt, ...) {
    if (!status) {
        return;
    }
    char    text[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    lv_label_set_text(status, text);
}

static void start_scan(void);

// ---- joining -----------------------------------------------------------------------------------------------------

static void stop_poll(void) {
    if (poll) {
        lv_timer_delete(poll);
        poll = NULL;
    }
}

static void run_done(void *fn) {
    ((void (*)(void))fn)();
}

static void on_poll(lv_timer_t *timer) {
    (void)timer;
    char tried[33], kept[33], a[140], b[140], ip[16] = "";
    if (!shell_sources.wifi_join_state) {
        stop_poll();
        return;
    }
    wifi_join_state_t st = shell_sources.wifi_join_state(tried, sizeof(tried), kept, sizeof(kept));
    wifi_ssid_sanitize(tried, a, sizeof(a));
    wifi_ssid_sanitize(kept, b, sizeof(b));
    if (st == WIFI_JOIN_TRYING || st == WIFI_JOIN_PENDING) {
        set_status("Connecting to %s...", a);
        return;
    }
    stop_poll();
    if (st == WIFI_JOIN_JOINED) {
        if (shell_sources.wifi_ip && shell_sources.wifi_ip(ip, sizeof(ip))) {
            set_status("Connected to %s, IP %s", a, ip);
        } else {
            set_status("Connected to %s", a);
        }
        if (setup_done) {
            lv_async_call(run_done, setup_done); // the screen goes: not from inside its timer
            setup_done = NULL;
        }
    } else if (st == WIFI_JOIN_FAILED_KEPT_OLD) {
        set_status(kept[0] ? "Couldn't join %s: kept %s" : "Couldn't join %s: no network saved", a, b);
    }
}

static void begin_join(char const *pass) {
    if (!shell_sources.wifi_join || !shell_sources.wifi_join(target, pass)) {
        set_status("Could not save or apply the settings");
        return;
    }
    // The request only queues the trial for c6link's own task; the outcome is c6link's, not this screen's
    set_status("Connecting to %s...", target);
    if (!poll) {
        poll = lv_timer_create(on_poll, POLL_MS, NULL);
    }
}

// The entry screen is gone by now (text_entry.h)
static void on_password(char const *text, void *arg) {
    (void)arg;
    if (!screen) {
        return;
    }
    if (!wifi_cred_valid(target, text)) {
        set_status("A password has 8 to 63 characters");
        return;
    }
    begin_join(text);
}

static void on_pick(void *arg) {
    size_t i = (size_t)(uintptr_t)arg;
    if (i >= shown_n) {
        return;
    }
    snprintf(target, sizeof(target), "%s", shown[i].ssid);
    if (shown[i].open) {
        begin_join("");
        return;
    }
    char shown_name[140], title[160];
    wifi_ssid_sanitize(target, shown_name, sizeof(shown_name));
    snprintf(title, sizeof(title), "Password for %.60s", shown_name);
    text_entry_open(title, "", true, 63, on_password, NULL);
}

// ---- the list ----------------------------------------------------------------------------------------------------

// Lock label and four signal bars on the right of a network row
static void decorate(lv_obj_t *row, wifi_ap_t const *ap) {
    lv_obj_t *name = lv_obj_get_child(row, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(name, 1);

    lv_obj_t *box = lv_obj_create(row);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, 30);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(box, 4, 0);
    lv_obj_set_scrollable(box, false);
    lv_obj_set_clickable(box, false);
    lv_obj_t *lock = lv_label_create(box);
    lv_obj_set_style_text_color(lock, lv_color_hex(THEME_DIM), 0);
    lv_obj_set_style_pad_right(lock, 12, 0);
    lv_label_set_text(lock, ap->open ? "open" : "locked");
    int bars = wifi_bars(ap->rssi);
    for (int b = 0; b < 4; b++) {
        lv_obj_t *bar = lv_obj_create(box);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 8, 8 + 6 * b);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(b < bars ? THEME_AMBER : THEME_FAINT), 0);
    }
}

static void on_skip(void *arg) {
    (void)arg;
    if (setup_done) {
        void (*done)(void) = setup_done;
        setup_done         = NULL;
        done();
    }
}

static void again(void *arg) {
    (void)arg;
    start_scan();
}

// Replaces the rows with "Scanning..." and starts the scan task
static void clear_rows(void) {
    for (size_t i = 0; i < row_count; i++) {
        lv_obj_delete(rows[i]);
    }
    row_count = 0;
}

static void on_scan_done(void *data) {
    scan_result_t *res = data;
    if (!screen || res->gen != gen) { // Esc was pressed (or the screen was reopened) while the scan ran
        free(res);
        return;
    }
    clear_rows();
    lv_group_t *previous = lv_group_get_default();
    lv_group_set_default(group);
    shown_n = res->ok ? res->n : 0;
    memcpy(shown, res->aps, shown_n * sizeof(shown[0]));
    for (size_t i = 0; i < shown_n; i++) {
        char name[140];
        wifi_ssid_sanitize(shown[i].ssid, name, sizeof(name));
        lv_obj_t *row = menu_add(menu, name, "", on_pick, (void *)(uintptr_t)i);
        if (row) {
            decorate(row, &shown[i]);
            rows[row_count++] = row;
        }
    }
    rows[row_count++] = menu_add(menu, "Scan again", "", again, NULL);
    lv_group_set_default(previous);
    menu_focus_row(menu, setup_done ? 1 : 0); // the first network, not Skip
    if (!res->ok) {
        set_status("The scan failed. Try again.");
    } else if (shown_n == 0) {
        set_status("No networks found.");
    } else if (!poll) { // a join in progress keeps its own status
        char ssid[33], name[140];
        settings_get_str("wifi_ssid", ssid, sizeof(ssid), "");
        wifi_ssid_sanitize(ssid, name, sizeof(name));
        char ip[16] = "";
        if (ssid[0] && shell_sources.wifi_ip && shell_sources.wifi_ip(ip, sizeof(ip))) {
            set_status("Connected to %s, IP %s", name, ip);
        } else {
            set_status("Choose a network");
        }
    }
    free(res);
}

static void scan_task(void *arg) {
    scan_result_t *res = calloc(1, sizeof(*res));
    if (res) {
        res->gen = (uint32_t)(uintptr_t)arg;
        res->ok  = shell_sources.wifi_scan && shell_sources.wifi_scan(res->aps, MAX_APS, &res->n);
        // lv_async_call is not thread safe: take the LVGL lock like the shell does
        lvgl_port_lock(0);
        lv_async_call(on_scan_done, res);
        lvgl_port_unlock();
    }
    vTaskDelete(NULL);
}

static void start_scan(void) {
    clear_rows();
    lv_group_t *previous = lv_group_get_default();
    lv_group_set_default(group);
    rows[row_count++] = menu_add(menu, "Scanning...", "", NULL, NULL); // keeps the group's focus on the list
    lv_group_set_default(previous);
    set_status("Scanning for networks...");
    if (xTaskCreatePinnedToCore(scan_task, "wifiscan", 4096, (void *)(uintptr_t)gen, 4, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "scan task did not start");
        set_status("Could not start the scan");
    }
}

// ---- the screen --------------------------------------------------------------------------------------------------

static void on_delete(lv_event_t *e) {
    (void)e;
    stop_poll();
    gen++; // a scan still running is dropped when it reports
    setup_done = NULL;
    screen = menu = status = NULL;
    group                  = NULL;
    row_count              = 0;
}

static lv_obj_t *build(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    shell_style_screen(scr);
    lv_obj_set_style_pad_top(scr, THEME_STATUS_BAR_H + THEME_PAD, 0);
    lv_obj_set_style_pad_hor(scr, THEME_PAD, 0);
    screen = scr;
    group  = lv_group_get_default(); // shell_open makes the screen's own group the default while this runs
    gen++;
    row_count = 0;
    shown_n   = 0;
    menu      = menu_create(scr, "Wi-Fi");
    if (setup_done) {
        menu_add(menu, "Skip for now", "", on_skip, NULL);
    }
    status    = menu_note(menu, "");
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);
    start_scan();
    wifi_join_state_t now = shell_sources.wifi_join_state ? shell_sources.wifi_join_state(target, sizeof(target), NULL, 0)
                                                          : WIFI_JOIN_NONE;
    if (now == WIFI_JOIN_TRYING || now == WIFI_JOIN_PENDING) {
        poll = lv_timer_create(on_poll, POLL_MS, NULL); // a trial started earlier is still running
    }
    return scr;
}

void wifi_screen_open(void) {
    shell_open(build, SCREEN_WIFI);
}

void wifi_screen_open_setup(void (*done)(void)) {
    setup_done = done; // build() reads it; on_delete clears it
    shell_open(build, SCREEN_WIFI);
    if (!screen) {
        setup_done = NULL; // not opened
    }
}
