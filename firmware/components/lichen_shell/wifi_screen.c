// Settings > Wi-Fi (#66): scan on core 0, pick a network, type the password, join. The scan blocks for seconds over
// ESP-Hosted, so it never runs on the LVGL task; its result comes back through lv_async_call and is dropped if the
// screen is gone by then.
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_radio.h"
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
static lv_obj_t   *focus_at_scan; // what had the focus when the scan started: compared, never dereferenced
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
static void clear_rows(void);

// The Wi-Fi radio switch (#108) is the first row: off clears the list, on scans once the driver has had a moment to start
static lv_obj_t   *radio_row;
static lv_timer_t *start_timer;
#define RADIO_START_MS 2000

static void radio_row_refresh(void) {
    if (radio_row) {
        menu_set_on(radio_row, lichen_radio_get(RADIO_WIFI));
    }
}

static void scan_after_start(lv_timer_t *timer) {
    lv_timer_delete(timer);
    start_timer = NULL;
    if (screen && lichen_radio_get(RADIO_WIFI)) {
        start_scan();
    }
}

static void toggle_radio(void *arg) {
    (void)arg;
    bool on = !lichen_radio_get(RADIO_WIFI);
    lichen_radio_set(RADIO_WIFI, on);
    radio_row_refresh();
    gen++; // a scan under way is dropped when it reports
    if (start_timer) {
        lv_timer_delete(start_timer);
        start_timer = NULL;
    }
    clear_rows();
    if (on) {
        set_status("Starting Wi-Fi...");
        start_timer = lv_timer_create(scan_after_start, RADIO_START_MS, NULL);
    } else {
        set_status("Wi-Fi is off");
    }
}

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
        // Why (it did not accept us: usually the password) and what to do (pick it again), and what is still in use
        set_status(kept[0] ? "Couldn't join %s: wrong password or out of range. Still using %s. Pick the network "
                             "again to retry."
                           : "Couldn't join %s: wrong password or out of range. Pick the network again to retry.",
                   a, b);
    }
}

static void begin_join(char const *pass) {
    if (!shell_sources.wifi_join || !shell_sources.wifi_join(target, pass)) {
        set_status("Could not save the network. Try again.");
        return;
    }
    // The request only queues the trial for c6link's own task; the outcome is c6link's, not this screen's
    set_status("Connecting to %s...", target);
    if (!poll) {
        poll = lv_timer_create(on_poll, POLL_MS, NULL);
    }
}

static void ask_password(char const *initial, bool too_short);

// The entry screen is gone by now (text_entry.h)
static void on_password(char const *text, void *arg) {
    (void)arg;
    if (!screen) {
        return;
    }
    if (!wifi_cred_valid(target, text)) {
        ask_password(text, true); // type again instead of picking the network again (text_entry copies initial now)
        return;
    }
    begin_join(text);
}

static void ask_password(char const *initial, bool wrong_length) {
    char shown_name[140], title[200];
    wifi_ssid_sanitize(target, shown_name, sizeof(shown_name));
    snprintf(title, sizeof(title), wrong_length ? "Password for %.60s: use 8 to 63 characters" : "Password for %.60s",
             shown_name);
    text_entry_open(title, initial, true, 63, on_password, NULL);
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
    ask_password("", false);
}

// ---- the list ----------------------------------------------------------------------------------------------------

// Lock label and four signal bars on the right of a network row
static void decorate(lv_obj_t *row, wifi_ap_t const *ap, bool connected) {
    lv_obj_t *name = lv_obj_get_child(row, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(name, 1);
    // The row's empty value label would take half the free width as well (#131)
    lv_obj_add_flag(lv_obj_get_child(row, 1), LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *box = lv_obj_create(row);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, 30);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(box, 4, 0);
    lv_obj_set_scrollable(box, false);
    lv_obj_set_clickable(box, false);
    if (connected) { // a word as well as the colour (R8)
        lv_obj_t *mark = lv_label_create(box);
        lv_obj_set_style_text_color(mark, lv_color_hex(THEME_TEAL), 0);
        lv_obj_set_style_pad_right(mark, 12, 0);
        lv_label_set_text(mark, LV_SYMBOL_OK " connected");
    }
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
    // The user moved while the scan ran (to Skip, say): the result does not take the focus from them (#128)
    lv_obj_t *now   = group ? lv_group_get_focused(group) : NULL;
    bool      moved = now != focus_at_scan && !(row_count > 0 && now == rows[0]);
    clear_rows();
    lv_group_t *previous = lv_group_get_default();
    lv_group_set_default(group);
    shown_n = res->ok ? res->n : 0;
    memcpy(shown, res->aps, shown_n * sizeof(shown[0]));
    char joined[33] = "", ip[16] = "";
    settings_get_str("wifi_ssid", joined, sizeof(joined), "");
    bool online = joined[0] && lichen_radio_get(RADIO_WIFI) && shell_sources.wifi_ip && shell_sources.wifi_ip(ip, sizeof(ip));
    for (size_t i = 0; i < shown_n; i++) {
        char name[140];
        wifi_ssid_sanitize(shown[i].ssid, name, sizeof(name));
        lv_obj_t *row = menu_add(menu, name, "", on_pick, (void *)(uintptr_t)i);
        if (row) {
            decorate(row, &shown[i], online && strcmp(shown[i].ssid, joined) == 0);
            rows[row_count++] = row;
        }
    }
    rows[row_count++] = menu_add(menu, "Scan again", "", again, NULL);
    lv_group_set_default(previous);
    if (!moved && rows[0]) {
        lv_group_focus_obj(rows[0]); // the first network or "Scan again": never the radio switch, one Enter from off
    }
    if (!res->ok) {
        set_status("The scan failed. Try again.");
    } else if (shown_n == 0) {
        set_status("No networks found.");
    } else if (!poll) { // a join in progress keeps its own status
        char name[140];
        wifi_ssid_sanitize(joined, name, sizeof(name));
        if (online) {
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
    focus_at_scan = group ? lv_group_get_focused(group) : NULL;
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
    if (start_timer) {
        lv_timer_delete(start_timer);
        start_timer = NULL;
    }
    radio_row = NULL;
    gen++; // a scan still running is dropped when it reports
    setup_done = NULL;
    screen = menu = status = NULL;
    group                  = NULL;
    focus_at_scan          = NULL;
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
    radio_row = menu_add(menu, "Wi-Fi radio", "", toggle_radio, NULL);
    menu_set_kind(radio_row, MENU_TOGGLE);
    menu_set_on(radio_row, lichen_radio_get(RADIO_WIFI));
    status    = menu_note(menu, "");
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);
    if (lichen_radio_get(RADIO_WIFI)) {
        start_scan();
    } else {
        set_status(lichen_airplane_get() ? "Wi-Fi is off (airplane mode)" : "Wi-Fi is off");
    }
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
