// Settings (#66, settings.html top row): Wi-Fi, Display (brightness, home layout), Time zone, About Lichen, then one
// entry per app that has a settings page. Every screen is a menu.h list; Esc is the shell's.
#include "app_icons.h"
#include "backlight.h"
#include "backlight_level.h"
#include "home_layout.h"
#include "lichen_app.h"
#include "menu.h"
#include "settings_store.h"
#include "shell_parts.h"
#include "theme.h"
#include "tz_table.h"
#include "wifi_list.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SCREEN_DISPLAY 101
#define SCREEN_TZ      102
#define SCREEN_ABOUT   103
#define SCREEN_APP     110 // + app index
#define SAVE_AFTER_MS  1000 // brightness is saved this long after the last step, to spare the flash

// ---- tile text and the Wi-Fi line --------------------------------------------------------------------------------

static void wifi_line(char *out, size_t cap, bool with_prefix) {
    static char ssid[33];
    static bool     have, was_up;
    static uint32_t generation;
    bool            up = shell_sources.wifi_up && shell_sources.wifi_up();
    // The NVS read only when something changed: the link flipped or the credentials were saved (screen or console)
    if (!have || up != was_up || generation != wifi_cred_generation()) {
        settings_get_str("wifi_ssid", ssid, sizeof(ssid), "");
        have       = true;
        was_up     = up;
        generation = wifi_cred_generation();
    }
    if (up && ssid[0]) {
        char name[140]; // the name came over the air: no control bytes on a label
        wifi_ssid_sanitize(ssid, name, sizeof(name));
        snprintf(out, cap, with_prefix ? "Wi-Fi: %s" : "%s", name);
    } else {
        snprintf(out, cap, with_prefix ? "Wi-Fi: not connected" : "not connected");
    }
}

static bool settings_tile(char *l1, size_t c1, char *l2, size_t c2) {
    wifi_line(l1, c1, true);
    l2[0] = '\0';
    (void)c2;
    return true;
}

// ---- screens -----------------------------------------------------------------------------------------------------

lv_obj_t *shell_page_create(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    shell_style_screen(screen);
    lv_obj_set_style_pad_top(screen, THEME_STATUS_BAR_H + THEME_PAD, 0);
    lv_obj_set_style_pad_hor(screen, THEME_PAD, 0);
    shell_scroll_tame(screen);
    return screen;
}

static void open_wifi(void *arg) {
    (void)arg;
    wifi_screen_open();
}

// Display: brightness is applied on every step and saved once it has settled
static lv_timer_t *save_timer;
static uint8_t     level[2]; // screen, keyboard
static bool        layout_changed;

static void save_levels(void) {
    backlight_set_saved(level[0], level[1]);
}

static void save_due(lv_timer_t *timer) {
    (void)timer;
    save_timer = NULL; // one-shot: LVGL deletes it after this call
    save_levels();
}

static void on_level(int value, void *arg) {
    level[(intptr_t)arg] = (uint8_t)value;
    backlight_set(level[0], level[1]);
    if (save_timer) {
        lv_timer_reset(save_timer);
    } else {
        save_timer = lv_timer_create(save_due, SAVE_AFTER_MS, NULL);
        lv_timer_set_repeat_count(save_timer, 1);
    }
}

static lv_obj_t *layout_row;

static char const *layout_name(void) {
    return settings_get_u8("home_layout", HOME_TILES) == HOME_GRID ? "Grid" : "Tiles";
}

static void toggle_layout(void *arg) {
    (void)arg;
    bool grid = settings_get_u8("home_layout", HOME_TILES) != HOME_GRID;
    settings_set_u8("home_layout", grid ? HOME_GRID : HOME_TILES);
    menu_set_value(layout_row, layout_name());
    layout_changed = true; // home is rebuilt when this screen is left: a rebuild now would close it
}

static void rebuild_later(void *arg) {
    (void)arg;
    shell_rebuild_home();
}

static void on_display_delete(lv_event_t *e) {
    (void)e;
    if (save_timer) {
        lv_timer_delete(save_timer);
        save_timer = NULL;
        save_levels();
    }
    layout_row = NULL;
    if (layout_changed) {
        layout_changed = false;
        lv_async_call(rebuild_later, NULL); // after the screen being deleted is gone
    }
}

static lv_obj_t *build_display(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "Display");
    backlight_get(&level[0], &level[1]);
    menu_add_slider(menu, "Screen", BACKLIGHT_DISPLAY_MIN, 100, level[0], on_level, (void *)(intptr_t)0);
    menu_add_slider(menu, "Keyboard", 0, 100, level[1], on_level, (void *)(intptr_t)1);
    if (shell_sources.c6_up && !shell_sources.c6_up()) {
        menu_note(menu, "brightness needs the C6");
    }
    layout_row = menu_add(menu, "Home layout", layout_name(), toggle_layout, NULL);
    lv_obj_add_event_cb(screen, on_display_delete, LV_EVENT_DELETE, NULL);
    return screen;
}

static void open_display(void *arg) {
    (void)arg;
    shell_open(build_display, SCREEN_DISPLAY);
}

// Time zone
static lv_obj_t *tz_row[16];
static int       tz_selected = -1;

static void pick_tz(void *arg) {
    int i = (int)(intptr_t)arg;
    settings_set_tz(tz_at((size_t)i)->posix);
    if (tz_selected >= 0 && tz_row[tz_selected]) {
        menu_set_value(tz_row[tz_selected], "");
    }
    menu_set_value(tz_row[i], LV_SYMBOL_OK);
    tz_selected = i;
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
    for (size_t i = 0; i < tz_count() && i < sizeof(tz_row) / sizeof(tz_row[0]); i++) {
        tz_row[i] = menu_add(menu, tz_at(i)->name, (int)i == tz_selected ? LV_SYMBOL_OK : "", pick_tz,
                             (void *)(intptr_t)i);
    }
    if (tz_selected >= 0) {
        menu_focus_row(menu, tz_selected);
    }
    lv_obj_add_event_cb(screen, on_tz_delete, LV_EVENT_DELETE, NULL);
    return screen;
}

static void open_tz(void *arg) {
    (void)arg;
    shell_open(build_tz, SCREEN_TZ);
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

// A page an app provides under Settings
static lichen_app_t const *page_app;

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
    lv_obj_t *menu   = menu_create(screen, page_app->name);
    if (page_app->settings) {
        page_app->settings(menu);
    }
    if (page_app->about) {
        page_app->about(menu);
    }
    lv_obj_add_event_cb(screen, on_page_key, LV_EVENT_KEY, menu);
    return screen;
}

static void open_app_page(void *arg) {
    size_t index = (size_t)(intptr_t)arg;
    page_app     = lichen_app_at(index);
    if (page_app && (page_app->settings || page_app->about)) {
        shell_open(build_app_page, SCREEN_APP + (int)index);
    }
}

static void open_settings(lv_obj_t *screen) {
    lv_obj_t *menu = menu_create(screen, "Settings");
    char      wifi[48];
    wifi_line(wifi, sizeof(wifi), false);
    menu_note(menu, "Lichen");
    menu_add(menu, "Wi-Fi", wifi, open_wifi, NULL);
    menu_add(menu, "Display", "", open_display, NULL);
    menu_add(menu, "Time zone", "", open_tz, NULL);
    menu_add(menu, "About Lichen", "", open_about, NULL);
    bool heading = false;
    for (size_t i = 0; i < lichen_app_count(); i++) {
        lichen_app_t const *app = lichen_app_at(i);
        if (!app->settings && !app->about) {
            continue;
        }
        if (!heading) {
            menu_note(menu, "Apps");
            heading = true;
        }
        menu_add(menu, app->name, "", open_app_page, (void *)(intptr_t)i);
    }
}

static lichen_app_t const settings_app = {
    .name      = "Settings",
    .draw_icon = icon_settings,
    .tile_text = settings_tile,
    .open      = open_settings,
};

void settings_app_register(void) {
    lichen_register_app(&settings_app);
}
