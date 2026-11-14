// Settings (#66, settings.html top row): Wi-Fi, Display (its own page, settings_display.c), Time (#64: set
// date & time, time zone), SD card (#99, sd_screen.c), Notifications (#62, notify_centre.c), Factory reset (#66, factory_screen.c), About Lichen, then one entry per app that has a settings page.
// Every screen is a menu.h list; Esc is the shell's.
#include "app_icons.h"
#include "app_page.h"
#include "lichen_app.h"
#include "lichen_sys_app.h"
#include "lichen_reg.h"
#include "lichen_notify.h"
#include "lichen_radio.h"
#include "lichen_sys_clock.h"
#include "lichen_sys_net.h"
#include "lichen_sys_psram.h"
#include "lichen_sys_radio.h"
#include "lichen_sys_storage.h"
#include "lichen_sys_update.h"
#include "menu.h"
#include "reg_row_text.h"
#include "screen_ids.h"
#include "settings_store.h"
#include "shell_parts.h"
#include "shell_ui_ops.h"
#include "theme.h"
#include "tz_table.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <time.h>


// ---- tile text and the Wi-Fi line --------------------------------------------------------------------------------

static void wifi_line(char *out, size_t cap, bool with_prefix) {
    static char ssid[33];
    static bool     have, was_up;
    static uint32_t generation;
    bool            up = lichen_sys_wifi_up() && lichen_radio_get(RADIO_WIFI);
    // The NVS read only when something changed: the link flipped or the credentials were saved (screen or console)
    if (!have || up != was_up || generation != lichen_sys_wifi_cred_generation()) {
        settings_get_str("wifi_ssid", ssid, sizeof(ssid), "");
        have       = true;
        was_up     = up;
        generation = lichen_sys_wifi_cred_generation();
    }
    bool held = lichen_sys_radio_held_by(RADIO_WIFI) == RADIO_HOLD_THERMAL;
    if (!lichen_radio_get(RADIO_WIFI) && held && lichen_radio_switch_get(RADIO_WIFI)) { // comes back on cooling (#348)
        snprintf(out, cap, with_prefix ? "Wi-Fi: held off (hot)" : "held off (hot)");
    } else if (!lichen_radio_get(RADIO_WIFI)) { // the OS switch or airplane mode (#108)
        snprintf(out, cap, with_prefix ? "Wi-Fi: off" : "off");
    } else if (up && ssid[0]) {
        char name[140]; // the name came over the air: no control bytes on a label
        lichen_sys_wifi_ssid_sanitize(ssid, name, sizeof(name));
        snprintf(out, cap, with_prefix ? "Wi-Fi: %s" : "%s", name);
    } else {
        snprintf(out, cap, with_prefix ? "Wi-Fi: not connected" : "not connected");
    }
}

static void wifi_value(char *out, size_t cap) {
    wifi_line(out, cap, false);
}

static bool settings_tile(char *l1, size_t c1, char *l2, size_t c2) {
    wifi_line(l1, c1, true);
    l2[0] = '\0';
    (void)c2;
    return true;
}

// ---- screens -----------------------------------------------------------------------------------------------------

lv_obj_t *shell_page_create_impl(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    shell_style_screen(screen);
    lv_obj_set_style_pad_top(screen, THEME_STATUS_BAR_H + THEME_PAD, 0);
    lv_obj_set_style_pad_hor(screen, THEME_PAD, 0);
    shell_scroll_tame(screen);
    shell_no_arrow_scroll(screen);
    return screen;
}

static void open_wifi(void *arg) {
    (void)arg;
    wifi_screen_open();
}

// The rows that show the time are refreshed once a second, and only redrawn when their text changed
void shell_live_update(shell_live_row_t *r) {
    if (!r->obj) {
        return; // its page is gone
    }
    char v[sizeof(r->shown)];
    r->fill(v, sizeof(v));
    if (strcmp(v, r->shown) != 0) {
        strlcpy(r->shown, v, sizeof(r->shown));
        if (r->note) {
            lv_label_set_text(r->obj, v);
        } else {
            menu_set_value(r->obj, v);
        }
    }
}

shell_live_row_t shell_live_row(lv_obj_t *obj, bool note, void (*fill)(char *, size_t)) {
    shell_live_row_t r = {.obj = obj, .note = note, .fill = fill};
    fill(r.shown, sizeof(r.shown));
    if (note) {
        lv_label_set_text(obj, r.shown);
    } else {
        menu_set_value(obj, r.shown);
    }
    return r;
}

// The registry rows (#332, #333): the applied value of a key from the cache (never waits for the C6), its age, and
// "saved ..." when it is not the saved one. reg_row_text builds the text; `saved` is NULL for a key with no saved twin
void shell_reg_text(char const *label, char const *key, char const *saved, char const *unit, char *out, size_t cap) {
    char cached[64];
    if (!lichen_reg_get(key, cached, sizeof(cached))) {
        cached[0] = '\0'; // a name the table does not have: shown as "?"
    }
    reg_row_text(label, cached, saved, unit, out, cap);
}

// Reads several keys' cached text into one set of buffers for the line builders
#define KEYS_MAX 5
typedef struct {
    char        text[KEYS_MAX][64];
    char const *ptr[KEYS_MAX];
} key_texts_t;

// One scratch for both lines, not a 320 B stack frame in the timer: only the LVGL task (its timers) calls them
static key_texts_t k;

static void read_keys(char const *const names[], size_t n, key_texts_t *dst) {
    for (size_t i = 0; i < n && i < KEYS_MAX; i++) {
        dst->ptr[i] = lichen_reg_get(names[i], dst->text[i], sizeof(dst->text[i])) ? dst->text[i] : NULL;
    }
}

void shell_lora_line(char *out, size_t cap) {
    static char const *const names[RR_LORA_KEYS] = {"lora.freq", "lora.bw", "lora.sf", "lora.cr", "lora.power"};
    read_keys(names, RR_LORA_KEYS, &k);
    reg_lora_line("LoRa now", k.ptr, out, cap);
}

void shell_wifi_line(char *out, size_t cap) {
    static char const *const names[RR_WIFI_KEYS] = {"wifi.country", "wifi.txpwr", "wifi.bw", "wifi.protocol", "wifi.ps"};
    read_keys(names, RR_WIFI_KEYS, &k);
    reg_wifi_line("Radio now", k.ptr, out, cap);
}

void shell_psram_line(char *out, size_t cap) {
    char clk[64], want[64] = "", want_mhz[16];
    if (!lichen_reg_get("psram.clk", clk, sizeof(clk))) {
        clk[0] = '\0';
    }
    if (lichen_reg_get("psram.want", want, sizeof(want))) {
        reg_row_value(want, want_mhz, sizeof(want_mhz));
    } else {
        want_mhz[0] = '\0';
    }
    reg_mhz_text("PSRAM now", clk, want_mhz, out, cap);
}

// Time zone
static void time_tz_changed(void); // Time's page under this one shows the zone

static lv_obj_t *tz_row[TZ_MAX];
static int       tz_selected = -1;

static void pick_tz(void *arg) {
    int i = (int)(intptr_t)arg;
    settings_set_tz(tz_at((size_t)i)->posix);
    if (i < 0 || i >= TZ_MAX || !tz_row[i]) {
        return;
    }
    if (tz_selected >= 0 && tz_selected < TZ_MAX && tz_row[tz_selected]) {
        menu_set_value(tz_row[tz_selected], "");
    }
    menu_set_value(tz_row[i], LV_SYMBOL_OK);
    tz_selected = i;
    time_tz_changed();
}

static void on_tz_delete(lv_event_t *e) {
    (void)e;
    memset(tz_row, 0, sizeof(tz_row));
}

static lv_obj_t *build_tz(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "Time zone");
    char      saved[64];
    settings_get_str("tz", saved, sizeof(saved), TZ_DEFAULT_POSIX);
    tz_selected = tz_find_posix(saved);
    for (size_t i = 0; i < tz_count() && i < TZ_MAX; i++) {
        tz_row[i] = menu_add(menu, tz_at(i)->name, (int)i == tz_selected ? LV_SYMBOL_OK : "", pick_tz,
                             (void *)(intptr_t)i);
    }
    if (tz_selected >= 0 && tz_selected < TZ_MAX) {
        menu_focus_row(menu, tz_selected);
    }
    lichen_ui_event_add(screen, on_tz_delete, LV_EVENT_DELETE, NULL);
    return screen;
}

static void open_tz(void *arg) {
    (void)arg;
    shell_open(build_tz, SCREEN_TZ);
}

// ---- Time (#64): Set date & time, Time zone, where the time came from ------------------------------------------

// Settings' row: "14:05", "~14:05" (approximate), "not set"; where the time came from is on the Time page
static void time_value(char *out, size_t cap) {
    time_t    now = time(NULL);
    struct tm l;
    char      hm[8];
    if (!lichen_sys_clock_is_set() || !localtime_r(&now, &l) || !strftime(hm, sizeof(hm), "%H:%M", &l)) {
        snprintf(out, cap, "not set");
        return;
    }
    snprintf(out, cap, "%s%s", lichen_sys_clock_is_approximate() ? "~" : "", hm);
}

// Set date & time's row: the local date and time
static void now_value(char *out, size_t cap) {
    time_t    now = time(NULL);
    struct tm l;
    if (!lichen_sys_clock_is_set() || !localtime_r(&now, &l) || !strftime(out, cap, "%Y-%m-%d %H:%M", &l)) {
        snprintf(out, cap, "not set");
    }
}

static void tz_value(char *out, size_t cap) {
    char posix[64];
    settings_get_str("tz", posix, sizeof(posix), TZ_DEFAULT_POSIX);
    int i = tz_find_posix(posix);
    snprintf(out, cap, "%s", i >= 0 ? tz_at((size_t)i)->name : "");
}

static shell_live_row_t time_rows[3]; // Time's page: Set date & time, Time zone, the source note
static shell_live_row_t settings_time_row; // Settings' own page: Time
static shell_live_row_t settings_sd_row;   // and SD card (a mount or unmount on its page or the console shows on return)
static shell_live_row_t settings_wifi_row;   // Wi-Fi (the radio switch is on its page)
static shell_live_row_t settings_lora_row;   // LoRa radio: the person's switch, held or not (#348)
static shell_live_row_t settings_notify_row; // and Notifications (do-not-disturb, #62)
static shell_live_row_t radio_notes[3];      // LoRa's settings, LoRa's state and the Bluetooth controller (#333)
static shell_live_row_t settings_psram_row;  // the PSRAM clock against the speed asked for (#333)
static shell_live_row_t settings_ota_row;    // and Updates over Wi-Fi (the auto-off timer closes it by itself, #112)

// The zone row reads NVS: only refreshed when a zone is picked, not every second
static void refresh_time_page(lv_timer_t *t) {
    (void)t;
    shell_live_update(&time_rows[0]);
    shell_live_update(&time_rows[2]);
}

static void time_tz_changed(void) {
    shell_live_update(&time_rows[1]);
}

static void refresh_settings_time(lv_timer_t *t) {
    (void)t;
    shell_live_update(&settings_time_row);
    shell_live_update(&settings_sd_row);
    shell_live_update(&settings_notify_row);
    shell_live_update(&settings_wifi_row);
    shell_live_update(&settings_ota_row);
    shell_live_update(&settings_lora_row);
    for (size_t i = 0; i < sizeof(radio_notes) / sizeof(radio_notes[0]); i++) {
        shell_live_update(&radio_notes[i]);
    }
    shell_live_update(&settings_psram_row);
}

// Each page's timer dies with it, and the rows it filled are forgotten
static void forget_time_page(lv_event_t *e) {
    lv_timer_delete(lv_event_get_user_data(e));
    memset(time_rows, 0, sizeof(time_rows));
}

static void forget_settings_time(lv_event_t *e) {
    lv_timer_delete(lv_event_get_user_data(e));
    memset(&settings_time_row, 0, sizeof(settings_time_row));
    memset(&settings_sd_row, 0, sizeof(settings_sd_row));
    memset(&settings_notify_row, 0, sizeof(settings_notify_row));
    memset(&settings_wifi_row, 0, sizeof(settings_wifi_row));
    memset(&settings_ota_row, 0, sizeof(settings_ota_row));
    memset(&settings_lora_row, 0, sizeof(settings_lora_row));
    memset(radio_notes, 0, sizeof(radio_notes));
    memset(&settings_psram_row, 0, sizeof(settings_psram_row));
}

static void source_note(char *out, size_t cap) {
    lichen_sys_clock_source_text(out, cap);
}

static void open_time_set(void *arg) {
    (void)arg;
    shell_open(time_set_build, SCREEN_TIME_SET);
}

static lv_obj_t *build_time(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "Time");
    lv_obj_t *set_row = menu_add(menu, "Set date & time", "", open_time_set, NULL);
    lv_obj_t *tz_open = menu_add(menu, "Time zone", "", open_tz, NULL);
    menu_set_kind(set_row, MENU_OPEN);
    menu_set_kind(tz_open, MENU_OPEN);
    time_rows[0]     = shell_live_row(set_row, false, now_value);
    time_rows[1]     = shell_live_row(tz_open, false, tz_value);
    time_rows[2]     = shell_live_row(menu_note(menu, ""), true, source_note);
    lichen_ui_event_add(screen, forget_time_page, LV_EVENT_DELETE, lichen_ui_timer_create(refresh_time_page, 1000, NULL));
    return screen;
}

static void open_time(void *arg) {
    (void)arg;
    shell_open(build_time, SCREEN_TIME);
}

// SD card's row: "28.1 GB free of 29.7 GB", "no card", ... (cached: never reads the card)
static void sd_value(char *out, size_t cap) {
    lichen_sd_info_t info;
    lichen_sys_sd_info(&info);
    lichen_sys_sd_short_text(&info, out, cap);
}

static void notify_value(char *out, size_t cap) {
    snprintf(out, cap, "%s", lichen_notify_dnd() ? "Do not disturb" : "");
}

static void open_notify(void *arg) {
    (void)arg;
    notify_settings_open();
}

static void open_sd(void *arg) {
    (void)arg;
    sd_screen_open();
}

static void open_about(void *arg) {
    (void)arg;
    // About is the app after Settings: open it as the app it is
    for (size_t i = 0; i < lichen_app_count(); i++) {
        if (strcmp(lichen_app_at(i)->name, "About Lichen") == 0) {
            shell_open_app(i);
            return;
        }
    }
}

// A page an app provides under Settings, and the app's context for its hooks
static lichen_app_t const *page_app;
static lichen_ctx_t        page_ctx;
static size_t              page_index; // its place in the OS's table (#233: the OS's rows on the page)

#define PAGE_SCROLL_STEP 96

// A page without rows (an app with only an about) has nothing to focus: the keys reach the screen and scroll the menu
static void on_page_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        shell_scroll_by(lv_event_get_user_data(e), key == LV_KEY_UP ? PAGE_SCROLL_STEP : -PAGE_SCROLL_STEP);
    }
}

static lv_obj_t *build_app_page(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create_named(screen, "App settings", page_app->name);
    lichen_ctx_t previous = lichen_ctx_enter(page_ctx); // #230: both hooks run for their app
    if (page_app->settings) {
        page_app->settings(menu);
    }
    if (page_app->about) {
        page_app->about(menu);
    }
    lichen_ctx_leave(previous);
    app_page_add_rows(screen, menu, page_index); // #233: after the app's own rows, so they keep their places
    lichen_ui_event_add(screen, on_page_key, LV_EVENT_KEY, menu);
    return screen;
}

static void open_app_page(void *arg) {
    size_t index = (size_t)(intptr_t)arg;
    page_app     = lichen_app_at(index);
    page_ctx     = lichen_sys_app_ctx_at(index);
    page_index   = index;
    if (page_app && app_page_listed(index)) {
        _Static_assert(LICHEN_MAX_APPS <= SCREEN_APP_MAX, "app page ids would overlap");
        shell_open(build_app_page, SCREEN_APP + (int)index);
    }
}

// Updates over Wi-Fi (#44): Enter switches the update server (and with it the Wi-Fi console) on or off
static lv_obj_t *ota_row, *ota_auto_row;

static char const *ota_value(void) {
    if (lichen_sys_update_refused()) {
        return "Off (no token)"; // #116: no token in /storage or settings this boot, refused until a reboot
    }
    return lichen_sys_update_enabled() ? "On" : lichen_sys_update_auto_closed() ? "Off (timer)" : "Off";
}

static void ota_fill(char *out, size_t cap) {
    strlcpy(out, ota_value(), cap);
}

static void ota_auto_value(char *out, size_t cap) {
    lichen_sys_update_auto_off_text(lichen_sys_update_auto_off(), out, cap);
}

static void ota_row_refresh(void) {
    if (ota_row) {
        menu_set_value(ota_row, ota_value());
    }
    if (ota_auto_row) {
        char text[16];
        ota_auto_value(text, sizeof(text));
        menu_set_value(ota_auto_row, text);
    }
}

// The optional auto-off (#110): Enter steps Never, 10 min, 30 min, 1 h, 2 h; it counts from now
static void step_ota_auto(void *arg) {
    (void)arg;
    lichen_sys_update_set_auto_off(lichen_sys_update_auto_off_next(lichen_sys_update_auto_off()));
    ota_row_refresh();
}

static void ota_off_answer(bool confirmed) {
    if (confirmed) {
        lichen_sys_update_set_enabled(false);
    }
    ota_row_refresh();
}

// Off cuts tools/badge.sh and the Wi-Fi console too, and only Settings or the USB console bring it back: confirm first
static void toggle_ota(void *arg) {
    (void)arg;
    if (!lichen_sys_update_enabled()) {
        lichen_sys_update_set_enabled(true);
        ota_row_refresh();
        return;
    }
    shell_dialog_t const d = {
        .title         = "Switch off updates over Wi-Fi? The Wi-Fi console goes too.",
        .seconds       = 10,
        .careful       = true,
        .confirm_label = "switch off",
    };
    shell_dialog_countdown(&d, ota_off_answer);
}

// Radio switches (#108): LoRa and airplane mode here, Wi-Fi on its own page. Airplane changes the others too, so every
// row is refreshed after a toggle. Both act at once without a question (#138): one Enter undoes them, the row shows
// the state, and turning a radio off or on starts no transmission by itself; a confirm here would only teach people
// to click through the careful dialogs that guard erasing.
// During a thermal hold the LoRa row shows and flips the person's switch only (#348): a press never lifts the hold, so
// a hot badge does not start transmitting; the row is a live one, so it follows the hold starting and ending.
static lv_obj_t *lora_row, *air_row;

void shell_radio_value(radio_id_t id, char *out, size_t cap) {
    snprintf(out, cap, "%s%s", lichen_radio_switch_get(id) ? "On" : "Off",
             lichen_sys_radio_held_by(id) == RADIO_HOLD_THERMAL ? ", held off (hot)" : "");
}

static void lora_value(char *out, size_t cap) {
    shell_radio_value(RADIO_LORA, out, cap);
}

static void radio_rows_refresh(void) {
    shell_live_update(&settings_wifi_row);
    shell_live_update(&settings_lora_row);
    if (air_row) {
        menu_set_on(air_row, lichen_airplane_get());
    }
}

static void toggle_lora(void *arg) {
    (void)arg;
    lichen_radio_switch_set(RADIO_LORA, !lichen_radio_switch_get(RADIO_LORA));
    radio_rows_refresh();
}

static void toggle_airplane(void *arg) {
    (void)arg;
    lichen_airplane_set(!lichen_airplane_get());
    radio_rows_refresh();
}

static void forget_ota_row(lv_event_t *e) {
    (void)e;
    ota_row = ota_auto_row = lora_row = air_row = NULL;
}

static void lora_state_fill(char *out, size_t cap) {
    shell_reg_text("LoRa state", "lora.state", NULL, NULL, out, cap);
}

static void bt_state_fill(char *out, size_t cap) {
    shell_reg_text("Bluetooth controller", "bt.state", NULL, NULL, out, cap); // the C6's, next to the airplane switch
}

// What the radios are actually set to (#333): applied values from the cache, read-only (changes stay on the guarded paths)
static void add_radio_notes(lv_obj_t *menu) {
    static void (*const fills[])(char *, size_t) = {shell_lora_line, lora_state_fill, bt_state_fill};
    for (size_t i = 0; i < sizeof(radio_notes) / sizeof(radio_notes[0]); i++) {
        radio_notes[i] = shell_live_row(menu_note(menu, ""), true, fills[i]);
    }
}

// A row that opens a page (R3): the dim "›" after the value
static lv_obj_t *open_row(lv_obj_t *menu, char const *label, void (*open)(void *)) {
    lv_obj_t *row = menu_add(menu, label, "", open, NULL);
    menu_set_kind(row, MENU_OPEN);
    return row;
}

// PSRAM speed (#121): 80 MHz is the default; 120 MHz is tried after a restart and falls back to 80 on its own
static lv_obj_t *psram_speed_row; // NULL once the page is gone

static void forget_psram_speed_row(lv_event_t *e) {
    (void)e;
    psram_speed_row = NULL;
}

static void psram_speed_enter(void *arg) {
    unsigned target = (unsigned)(uintptr_t)arg;
    if (lichen_sys_psram_speed_choice(target)) {
        power_request_restart(); // saves the apps, shows "Restarting...", lands in the new speed
    } else if (psram_speed_row) {
        menu_set_value(psram_speed_row, "Not saved"); // NVS refused, or this image is still on trial and a restart would undo it
    }
}

static void add_psram_speed_row(lv_obj_t *menu) {
    if (!lichen_sys_psram_speed_supported()) {
        return;
    }
    char value[40];
    lichen_sys_psram_speed_value(value, sizeof(value));
    unsigned other = lichen_sys_psram_speed_other();
    lv_obj_t *row  = menu_add(menu, "PSRAM speed", value, psram_speed_enter, (void *)(uintptr_t)other);
    psram_speed_row = row;
    lichen_ui_event_add(row, forget_psram_speed_row, LV_EVENT_DELETE, NULL);
    menu_set_danger(row, other == 120 ? "Switch PSRAM to 120 MHz? Restarts the badge. If 120 MHz fails, it comes back at 80."
                                       : "Switch PSRAM to 80 MHz? Restarts the badge.",
                    "restart");
}

static void open_settings(lv_obj_t *screen) {
    lv_obj_t *menu = menu_create(screen, "Settings");
    char      wifi[48];
    wifi_line(wifi, sizeof(wifi), false);
    menu_note(menu, "Lichen");
    settings_wifi_row = shell_live_row(menu_add(menu, "Wi-Fi", wifi, open_wifi, NULL), false, wifi_value);
    menu_set_kind(settings_wifi_row.obj, MENU_OPEN);
    strlcpy(settings_wifi_row.shown, wifi, sizeof(settings_wifi_row.shown));
    ota_row = menu_add(menu, "Updates over Wi-Fi", ota_value(), toggle_ota, NULL);
    menu_set_kind(ota_row, MENU_TOGGLE);
    settings_ota_row = shell_live_row(ota_row, false, ota_fill);
    char net_name[LICHEN_SYS_MDNS_NAME_MAX + 8];
    snprintf(net_name, sizeof(net_name), "%s.local", lichen_sys_mdns_name());
    menu_add(menu, "Name on the network", net_name, NULL, NULL); // for badge.sh and the browser (#156)
    char auto_text[16];
    ota_auto_value(auto_text, sizeof(auto_text));
    ota_auto_row = menu_add(menu, "Close updates after", auto_text, step_ota_auto, NULL);
    menu_set_kind(ota_auto_row, MENU_CYCLE);
    lora_row = menu_add(menu, "LoRa radio", "", toggle_lora, NULL);
    air_row  = menu_add(menu, "Airplane mode", "", toggle_airplane, NULL);
    menu_set_kind(lora_row, MENU_TOGGLE);
    menu_set_kind(air_row, MENU_TOGGLE);
    settings_lora_row = shell_live_row(lora_row, false, lora_value);
    menu_set_on(air_row, lichen_airplane_get());
    add_radio_notes(menu);
    lichen_ui_event_add(screen, forget_ota_row, LV_EVENT_DELETE, NULL);
    open_row(menu, "Display", settings_display_open);
    lv_obj_t *time_open = open_row(menu, "Time", open_time);
    settings_time_row = shell_live_row(time_open, false, time_value);
    lichen_ui_event_add(screen, forget_settings_time, LV_EVENT_DELETE,
                        lichen_ui_timer_create(refresh_settings_time, 1000, NULL));
    settings_sd_row = shell_live_row(open_row(menu, "SD card", open_sd), false, sd_value);
    settings_notify_row = shell_live_row(open_row(menu, "Notifications", open_notify), false, notify_value);
    open_row(menu, "Battery", battery_open);
    lv_obj_t *power_open = open_row(menu, "Power", power_open_from_settings);
    menu_set_value(power_open, "Restart, power off");
    lv_obj_t *backup_open = open_row(menu, "Backup", backup_screen_open);
    menu_set_value(backup_open, "Settings on SD card");
    open_row(menu, "Factory reset", factory_screen_open);
    open_row(menu, "About Lichen", open_about);
    menu_note(menu, "For developers");
    menu_set_value(open_row(menu, "Console", console_open_from_settings), "Run fw commands");
    add_psram_speed_row(menu);
    settings_psram_row = shell_live_row(menu_note(menu, ""), true, shell_psram_line);
    bool heading = false;
    for (size_t i = 0; i < lichen_app_count(); i++) {
        lichen_app_t const *app = lichen_app_at(i);
        if (!app_page_listed(i)) {
            continue;
        }
        if (!heading) {
            menu_note(menu, "Apps");
            heading = true;
        }
        menu_set_kind(menu_add(menu, app->name, "", open_app_page, (void *)(intptr_t)i), MENU_OPEN);
    }
}

static lichen_app_t const settings_app = {
    .name      = "Settings",
    .id        = "settings",
    .draw_icon = icon_settings,
    .tile_text = settings_tile,
    .open      = open_settings,
    .caps      = CAP_SYSTEM, // the shell's own app (#229)
};

void settings_app_register(void) {
    lichen_register_app(&settings_app);
}
