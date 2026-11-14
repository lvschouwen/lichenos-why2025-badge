// Settings > Display (split out of app_settings.c, #397): brightness sliders and what the C6 applied, message light,
// flashlight, screen timeout, screensaver and pocket lock (#75, #119), home layout, arrange home and reset order (#189).
#include "attention_policy.h"
#include "home_layout.h"
#include "idle_policy.h"
#include "lichen_reg.h"
#include "lichen_sys_c6.h"
#include "lichen_sys_display.h"
#include "lichen_sys_ui.h"
#include "lock_combo.h"
#include "menu.h"
#include "reg_row_text.h"
#include "saver_station.h"
#include "screen_ids.h"
#include "settings_store.h"
#include "shell_parts.h"
#include "shell_ui_ops.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SAVE_AFTER_MS 1000 // brightness is saved this long after the last step, to spare the flash

// Display: brightness is applied on every step and saved once it has settled
static lv_timer_t *save_timer;
static uint8_t     level[2]; // screen, keyboard
static uint8_t     layout_at_open; // home is rebuilt on leaving only when the layout really differs from this (#128)

static void save_levels(void) {
    lichen_sys_display_set_saved(level[0], level[1]);
}

static void save_due(lv_timer_t *timer) {
    (void)timer;
    save_timer = NULL; // one-shot: LVGL deletes it after this call
    save_levels();
}

static void on_level(int value, void *arg) {
    level[(intptr_t)arg] = (uint8_t)value;
    lichen_sys_display_set(level[0], level[1]);
    if (save_timer) {
        lv_timer_reset(save_timer);
    } else {
        save_timer = lichen_ui_timer_create(save_due, SAVE_AFTER_MS, NULL);
        lv_timer_set_repeat_count(save_timer, 1);
    }
}

// Message light (#164): the keyboard light blinks twice for a new message; Enter steps Off / Direct messages / All
static lv_obj_t *pulse_row;

static attn_mode_t pulse_mode(void) {
    return attn_mode_from_u8(settings_get_u8(ATTN_MODE_KEY, ATTN_MODE_DEFAULT));
}

static void step_pulse(void *arg) {
    (void)arg;
    settings_set_u8(ATTN_MODE_KEY, (uint8_t)attn_mode_next(pulse_mode()));
    menu_set_value(pulse_row, attn_mode_name(pulse_mode()));
}

static void open_flashlight(void *arg) {
    (void)arg;
    flashlight_open();
}

static lv_obj_t *layout_row;

static char const *layout_name(void) {
    return settings_get_u8("home_layout", HOME_TILES) == HOME_GRID ? "Grid" : "Tiles";
}

static void toggle_layout(void *arg) {
    (void)arg;
    bool grid = settings_get_u8("home_layout", HOME_TILES) != HOME_GRID;
    settings_set_u8("home_layout", grid ? HOME_GRID : HOME_TILES);
    menu_set_value(layout_row, layout_name()); // home is rebuilt when this screen is left, once, if still changed
}

// Screen timeout (#43): Enter steps through the choices; saved at once, the idle clock starts again from it
static lv_obj_t *dim_row, *off_row;

static void idle_rows_refresh(void) {
    char text[16];
    idle_choice_text(settings_get_u8("idle_dim", IDLE_DIM_DEFAULT_10S), text, sizeof(text));
    menu_set_value(dim_row, text);
    idle_choice_text(settings_get_u8("idle_off", IDLE_OFF_DEFAULT_10S), text, sizeof(text));
    menu_set_value(off_row, text);
}

static void step_idle(void *arg) {
    bool        off = arg != NULL;
    char const *key = off ? "idle_off" : "idle_dim";
    uint8_t     now = settings_get_u8(key, off ? IDLE_OFF_DEFAULT_10S : IDLE_DIM_DEFAULT_10S);
    settings_set_u8(key, off ? idle_off_next(now) : idle_dim_next(now));
    screen_idle_reload();
    idle_rows_refresh();
}

// The clock screensaver (the dim stage) and the pocket lock (#75): Enter switches; saved at once
static lv_obj_t *saver_row, *style_row, *lock_row;

static void switch_rows_refresh(void) {
    menu_set_on(saver_row, settings_get_u8(IDLE_SAVER_KEY, 1));
    bool digital = settings_get_u8(IDLE_SAVER_STYLE_KEY, SAVER_STYLE_STATION) == SAVER_STYLE_DIGITAL;
    menu_set_value(style_row, digital ? "Digital" : "Station clock");
    menu_set_on(lock_row, settings_get_u8(IDLE_LOCK_KEY, 0));
}

static void toggle_switch(void *arg) {
    char const *key = arg ? IDLE_LOCK_KEY : IDLE_SAVER_KEY;
    settings_set_u8(key, !settings_get_u8(key, arg ? 0 : 1));
    screen_idle_reload();
    switch_rows_refresh();
}

// The screensaver's look (#119): the station clock or the seven-segment one
static void toggle_style(void *arg) {
    (void)arg;
    bool digital = settings_get_u8(IDLE_SAVER_STYLE_KEY, SAVER_STYLE_STATION) == SAVER_STYLE_DIGITAL;
    settings_set_u8(IDLE_SAVER_STYLE_KEY, digital ? SAVER_STYLE_STATION : SAVER_STYLE_DIGITAL);
    screen_idle_reload();
    switch_rows_refresh();
}

// Arrange home (#189): Settings closes and home takes the keys in arrange mode
static void arrange_later(void *arg) {
    shell_arrange_home(arg != NULL);
}

static void open_arrange(void *arg) {
    (void)arg;
    uint8_t now      = settings_get_u8("home_layout", HOME_TILES);
    bool    relayout = now != layout_at_open;
    layout_at_open   = now; // on_display_delete's own rebuild would come after and replace the arranging home
    // Outside this row's key handling: the page is deleted
    lichen_sys_ui_call(arrange_later, relayout ? (void *)1 : NULL, 0);
}

static lv_obj_t *order_row;

// The row's value is the state (R3) and so also the answer to a reset (R5): "Arranged" or "Default"
static void order_row_refresh(void) {
    if (order_row) {
        menu_set_value(order_row, home_order_arranged() ? "Arranged" : "Default");
    }
}

static void reset_order_answer(bool yes) {
    if (yes) {
        settings_erase(HOME_ORDER_KEY);
        shell_rebuild_home_below(); // Settings stays on top
        order_row_refresh();
    }
}

static void ask_reset_order(void *arg) {
    (void)arg;
    shell_dialog_t const d = {
        .title         = "Reset the home order? The tiles go back to their first places.",
        .seconds       = 10,
        .careful       = true, // an arrangement takes work to redo: running out of time cancels
        .confirm_label = "reset",
    };
    shell_dialog_countdown(&d, reset_order_answer);
}

static void rebuild_later(void *arg) {
    (void)arg;
    shell_rebuild_home_below(); // Settings stays on top
}

// What the badge shows now (#332): the backlight as the C6 applied it, next to the sliders (which hold the saved levels)
static lv_timer_t *bl_timer;
static shell_live_row_t  bl_rows[5]; // screen, keyboard, light mode, override, boot light

static void bl_saved_fill(char const *label, char const *key, int which, char *out, size_t cap) {
    uint8_t lv[2];
    char    saved[8];
    lichen_sys_display_get(&lv[0], &lv[1]);
    snprintf(saved, sizeof(saved), "%u", lv[which]);
    shell_reg_text(label, key, saved, "%", out, cap);
}

static void bl_screen_fill(char *out, size_t cap) {
    bl_saved_fill("Screen now", "bl.display", 0, out, cap);
}

static void bl_keyboard_fill(char *out, size_t cap) {
    bl_saved_fill("Keyboard now", "bl.keyboard", 1, out, cap);
}

static void bl_idle_fill(char *out, size_t cap) {
    shell_reg_text("Timeout", "bl.idle", NULL, NULL, out, cap);
}

static void bl_override_fill(char *out, size_t cap) {
    shell_reg_text("Override", "bl.override", NULL, NULL, out, cap);
}

// Only while the boot light is still on: empty (the note is hidden) when it is off or was not read
static void bl_boot_fill(char *out, size_t cap) {
    char cached[64], value[16];
    if (!lichen_reg_get("bl.boot", cached, sizeof(cached)) || !reg_row_value(cached, value, sizeof(value)) ||
        strcmp(value, "on") != 0) {
        out[0] = '\0';
        return;
    }
    shell_reg_text("Boot light", "bl.boot", NULL, NULL, out, cap);
}

static void bl_refresh(lv_timer_t *t) {
    (void)t;
    for (size_t i = 0; i < sizeof(bl_rows) / sizeof(bl_rows[0]); i++) {
        shell_live_update(&bl_rows[i]);
    }
    if (bl_rows[4].obj) {
        if (bl_rows[4].shown[0]) {
            lv_obj_set_hidden(bl_rows[4].obj, false);
        } else {
            lv_obj_set_hidden(bl_rows[4].obj, true);
        }
    }
}

static void bl_rows_build(lv_obj_t *menu) {
    static void (*const fills[])(char *, size_t) = {bl_screen_fill, bl_keyboard_fill, bl_idle_fill, bl_override_fill,
                                                    bl_boot_fill};
    for (size_t i = 0; i < sizeof(bl_rows) / sizeof(bl_rows[0]); i++) {
        bl_rows[i] = shell_live_row(menu_note(menu, ""), true, fills[i]);
    }
    bl_refresh(NULL);
    bl_timer = lichen_ui_timer_create(bl_refresh, 1000, NULL);
}

static void on_display_delete(lv_event_t *e) {
    (void)e;
    if (save_timer) {
        lv_timer_delete(save_timer);
        save_timer = NULL;
        save_levels();
    }
    if (bl_timer) {
        lv_timer_delete(bl_timer);
        bl_timer = NULL;
    }
    memset(bl_rows, 0, sizeof(bl_rows));
    layout_row = NULL;
    order_row  = NULL;
    pulse_row  = NULL;
    dim_row    = NULL;
    off_row    = NULL;
    saver_row  = NULL;
    style_row  = NULL;
    lock_row   = NULL;
    if (settings_get_u8("home_layout", HOME_TILES) != layout_at_open) {
        lichen_sys_ui_call(rebuild_later, NULL, 0); // after the screen being deleted is gone
    }
}

static lv_obj_t *build_display(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "Display");
    lichen_sys_display_get(&level[0], &level[1]);
    menu_add_slider(menu, "Screen", LICHEN_SYS_DISPLAY_MIN, LICHEN_SYS_DISPLAY_MAX, level[0], on_level, (void *)(intptr_t)0);
    menu_add_slider(menu, "Keyboard", 0, LICHEN_SYS_DISPLAY_MAX, level[1], on_level, (void *)(intptr_t)1);
    bl_rows_build(menu);
    if (!lichen_sys_c6_up()) {
        menu_note(menu, "Brightness can't change right now: the radio chip is not answering. Restart the badge if it "
                        "stays like this.");
    }
    pulse_row = menu_add(menu, "Message light", attn_mode_name(pulse_mode()), step_pulse, NULL);
    menu_set_kind(pulse_row, MENU_CYCLE);
    menu_add(menu, "Flashlight", "", open_flashlight, NULL);
    dim_row    = menu_add(menu, "Dim after", "", step_idle, NULL);
    off_row    = menu_add(menu, "Screen off after", "", step_idle, (void *)1);
    menu_set_kind(dim_row, MENU_CYCLE);
    menu_set_kind(off_row, MENU_CYCLE);
    idle_rows_refresh();
    saver_row = menu_add(menu, "Clock screensaver", "", toggle_switch, NULL);
    style_row = menu_add(menu, "Screensaver style", "", toggle_style, NULL);
    lock_row  = menu_add(menu, "Pocket lock", "", toggle_switch, (void *)1);
    menu_set_kind(saver_row, MENU_TOGGLE);
    menu_set_kind(style_row, MENU_CYCLE);
    menu_set_kind(lock_row, MENU_TOGGLE);
    menu_note(menu, "Message light blinks the keyboard light for a new message (not under do-not-disturb or with the screen dark). Flashlight: white screen at full brightness until a key. The screensaver shows when the screen dims. The pocket lock comes on with it or when the screen goes "
                    "off; hold " LOCK_COMBO_TEXT " to unlock. It stops stray key presses, it is not security: no PIN.");
    switch_rows_refresh();
    layout_at_open = settings_get_u8("home_layout", HOME_TILES);
    layout_row     = menu_add(menu, "Home layout", layout_name(), toggle_layout, NULL);
    menu_set_kind(layout_row, MENU_CYCLE);
    menu_set_kind(menu_add(menu, "Arrange home", "", open_arrange, NULL), MENU_OPEN);
    order_row = menu_add(menu, "Reset order", "", ask_reset_order, NULL);
    order_row_refresh();
    shell_page_hint(screen, "Left/Right", "sliders", "Enter", "change", NULL);
    lichen_ui_event_add(screen, on_display_delete, LV_EVENT_DELETE, NULL);
    return screen;
}

void settings_display_open(void *arg) {
    (void)arg;
    shell_open(build_display, SCREEN_DISPLAY);
}
