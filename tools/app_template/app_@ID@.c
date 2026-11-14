// @NAME_C@ (scaffolded by `tools/badge.sh new-app`; the walk-through is docs/app-api.md "An app in 10 minutes").
// The screens and the saving of the counter in @ID@_model.c. Only the OS API: lichen_api headers, menu and theme.
#include "@ID@_model.h"
#include "lichen_@ID@.h"
#include "lichen_app.h"
#include "lichen_settings.h"
#include "lichen_str.h"
#include "lichen_ui.h"
#include "menu.h"
#include "theme.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Setting keys: 1..15 characters, in the app's own namespace (a.@ID@); another app's keys are out of reach
#define KEY_COUNT "count"
#define KEY_STEP  "step"

static lichen_ctx_t self_ctx; // what lichen_register_app gave back; every gate call (settings, memory, tasks) takes it
static @ID@_model_t s_m;
static bool         s_loaded;

// The open screen's widgets; NULL while it is closed (lichen_ui_event_add below clears them)
static struct {
    lv_obj_t *total;
} s_w;

// Read the saved values once, the first time anything needs them: register runs too early for settings
static void load(void) {
    if (s_loaded) {
        return;
    }
    @ID@_model_init(&s_m, app_settings_get_u32(self_ctx, KEY_COUNT, 0), app_settings_get_u32(self_ctx, KEY_STEP, 1));
    s_loaded = true;
}

static void show(void) {
    char text[32];
    @ID@_model_text(&s_m, text, sizeof(text));
    lichen_label_set(s_w.total, text); // sets the text only when it changed, so a redraw costs nothing
}

static void on_tap(void *arg) {
    (void)arg;
    if (@ID@_model_tap(&s_m)) {
        app_settings_set_u32(self_ctx, KEY_COUNT, s_m.count);
    }
    show();
}

static void on_reset(void *arg) {
    (void)arg;
    @ID@_model_init(&s_m, 0, s_m.step);
    app_settings_set_u32(self_ctx, KEY_COUNT, 0);
    show();
}

// An LVGL callback is registered through lichen_ui_event_add (never lv_obj_add_event_cb), so it runs as this app (#349)
static void on_screen_delete(lv_event_t *e) {
    (void)e;
    memset(&s_w, 0, sizeof(s_w));
}

static void open_app(lv_obj_t *screen) {
    load();
    memset(&s_w, 0, sizeof(s_w));
    lv_obj_t *menu = menu_create(screen, "@NAME_C@");
    menu_add(menu, "Tap", NULL, on_tap, NULL);
    s_w.total     = menu_note(menu, "");
    lv_obj_t *del = menu_add(menu, "Reset", NULL, on_reset, NULL);
    menu_set_kind(del, MENU_DANGER);
    menu_set_danger(del, "Reset the count?", "reset");
    lichen_ui_event_add(screen, on_screen_delete, LV_EVENT_DELETE, NULL);
    show();
}

// Settings > @NAME_C@: the OS made the page, the app adds its rows
static lv_obj_t *s_step_row;

static void step_show(void) {
    char value[8];
    snprintf(value, sizeof(value), "%u", (unsigned)s_m.step);
    menu_set_value(s_step_row, value);
}

static void on_step(void *arg) {
    (void)arg;
    s_m.step = @ID@_model_next_step(s_m.step);
    app_settings_set_u32(self_ctx, KEY_STEP, s_m.step);
    step_show();
}

static void app_settings(lv_obj_t *menu) {
    load();
    s_step_row = menu_add(menu, "Count by", "", on_step, NULL);
    menu_set_kind(s_step_row, MENU_CYCLE);
    step_show();
}

// Called every second from the LVGL task: quick, no waiting. false = nothing to show on the tile.
static bool app_tile(char *l1, size_t c1, char *l2, size_t c2) {
    load();
    if (s_m.count == 0) {
        return false;
    }
    @ID@_model_text(&s_m, l1, c1);
    lichen_str_copy(l2, c2, "on this badge");
    return true;
}

// 32 and 64 px: a rounded square with a bar in the brand's teal
static void app_icon(lv_obj_t *parent, int size_px) {
    lv_obj_set_style_radius(parent, size_px / 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lichen_icon_box(parent, size_px / 2, size_px / 8, 0, 0, THEME_TEAL, 2);
}

// The descriptor must stay valid for as long as the badge runs: static const. caps is what the app uses (none here;
// docs/app-api.md "Capabilities"); the budgets are KiB of internal RAM and PSRAM it may hold. Keep them equal to the
// `app` line this scaffold put in tools/layers.conf (rule D checks it).
static lichen_app_t const app = {
    .name             = "@NAME_C@",
    .id               = "@ID@",
    .draw_icon        = app_icon,
    .tile_text        = app_tile,
    .open             = open_app,
    .settings         = app_settings,
    .caps             = 0,
    .internal_ram_kib = 1,
    .psram_kib        = 0,
};

void @ID@_app_register(void) {
    self_ctx = lichen_register_app(&app);
}
