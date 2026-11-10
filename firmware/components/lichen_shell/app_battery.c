// Settings > Battery (#10): the honest page. The two 18650 cells are unprotected and sit behind a charger chip that has
// no data link to the processors, so the badge cannot see their voltage, charge or current; all it can do is watch
// the temperature (lichen_os/thermal.c). The same page, headed by a red warning, is the full-screen warning when the watchdog
// raises its alarm. Up/Down scroll; the live lines refresh once a second.
#include "esp_log.h"
#include "screen_ids.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_task.h"
#include "menu.h"
#include "shell_parts.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define SCROLL_STEP    96
#define REFRESH_MS     1000
#define ALERT_STACK    6144

static bool      alert_mode; // set before the builder runs

static void on_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        shell_scroll_by(lv_event_get_user_data(e), key == LV_KEY_UP ? SCROLL_STEP : -SCROLL_STEP);
    }
}

static void status_text(char *out, size_t cap) {
    lichen_sys_thermal_info_t i;
    lichen_sys_thermal_info(&i);
    char board[24] = "no reading", die[24] = "no reading";
    if (i.valid[TH_SRC_BOARD]) {
        snprintf(board, sizeof(board), "%.0f \xC2\xB0""C", (double)i.c[TH_SRC_BOARD]);
    }
    if (i.valid[TH_SRC_DIE]) {
        snprintf(die, sizeof(die), "%.0f \xC2\xB0""C", (double)i.c[TH_SRC_DIE]);
    }
    char const *state = i.level == TH_SHUTDOWN ? "powering off, too hot"
                        : i.level == TH_WARN   ? "warm, screen dimmed, radios off"
                        : i.blind              ? "no sensor answers, the heat is not watched"
                                               : "normal";
    snprintf(out, cap, "Temperature watch: %s\nBoard %s  \xE2\x80\xA2  processor %s\nWarns at 50 \xC2\xB0""C, powers off at 60 \xC2\xB0""C or on a "
                       "fast rise.", state, board, die);
}

static void refresh(lv_timer_t *timer) {
    lv_obj_t *label = lv_timer_get_user_data(timer);
    char      text[160];
    status_text(text, sizeof(text));
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void on_delete(lv_event_t *e) {
    lv_timer_delete(lv_event_get_user_data(e));
}

static void note_colour(lv_obj_t *note, uint32_t colour) {
    lv_obj_set_style_text_color(note, lv_color_hex(colour), 0);
}

static lv_obj_t *build_battery(void) {
    lv_obj_t *screen = shell_page_create();
    bool      alert  = alert_mode;
    alert_mode       = false;
    lv_obj_t *menu   = menu_create(screen, alert ? "Too hot" : "Battery");
    if (alert) {
        note_colour(menu_note(menu, "The badge is getting hot. The screen is dimmed to save power. Unplug the charger, "
                                    "put the badge down on something hard, away from anything that burns, and let it "
                                    "cool. If it keeps heating, take the batteries out."), THEME_RED);
    }
    lv_obj_t *status_label = menu_note(menu, "");
    note_colour(status_label, alert ? THEME_AMBER : THEME_TEXT);
    note_colour(menu_note(menu, "The two batteries in this badge have no protection circuit, and the charger chip "
                                "does not talk to the badge. Lichen cannot see their voltage, charge or current, and "
                                "cannot stop a short circuit or an overcharge."), THEME_AMBER);
    menu_note(menu, "What helps");
    menu_note(menu, "\xE2\x80\xA2 Power the badge from USB or a power bank when you can.");
    menu_note(menu, "\xE2\x80\xA2 Never leave it charging unattended or overnight, and not under a pillow or in a bag.");
    menu_note(menu, "\xE2\x80\xA2 Protected cells, or a protection board in line with the battery, are much safer; "
                    "the holders may not take protected cells.");
    menu_note(menu, "\xE2\x80\xA2 Keep metal away from the battery contacts. A short there can start a fire.");
    menu_note(menu, "\xE2\x80\xA2 Warm, swollen or smelly batteries: unplug, take them out, move away.");
    menu_note(menu, "The only thing Lichen can do is watch the temperature: it warns, dims the screen, and powers "
                    "the badge off when it gets too hot.");
    shell_page_hint(screen, "Up/Down", "scroll", NULL);
    lv_obj_add_event_cb(screen, on_key, LV_EVENT_KEY, menu);
    lv_timer_t *timer = lv_timer_create(refresh, REFRESH_MS, status_label);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, timer);
    refresh(timer);
    return screen;
}

void battery_open(void *arg) {
    (void)arg;
    alert_mode = false;
    shell_open(build_battery, SCREEN_BATTERY);
}

// Building the page takes the LVGL lock and some stack: a short task of its own, so the watchdog's task (4 KB) never
// waits for the UI or builds it
static void alert_task(void *arg) {
    (void)arg;
    alert_mode = true;
    shell_open(build_battery, SCREEN_BATTERY);
    screen_lock_wake(); // the warning must be seen: no lock or screensaver over it (#75)
}

void battery_open_alert(void) {
    if (!lichen_task_start(NULL, "battalert", alert_task, NULL, ALERT_STACK, LICHEN_PRIO_SERVICE, NULL)) {
        ESP_LOGW("battery", "no memory for the warning screen");
    }
}
