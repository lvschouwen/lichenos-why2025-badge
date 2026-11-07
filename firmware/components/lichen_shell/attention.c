// Keyboard light pulse for a new message, and the flashlight (#164). The OS owns the backlight: MeshCore only raises a
// notice (lichen_notify_to with its "dm:..." / "ch:..." target) and this file decides what the light does, so no app
// drives hardware. The pulse hooks the toast that comes with feedback (notify_ui.c), so do-not-disturb, LN_SILENT and
// the per-source rate are already honoured. Decisions are attention_policy.c (host-tested).
#include "attention_policy.h"
#include "backlight.h"
#include "backlight_level.h"
#include "esp_log.h"
#include "lichen_sys_ui.h"
#include "fw_console.h"
#include "lichen_notify.h"
#include "lichen_ui.h"
#include "lvgl.h"
#include "nav.h"
#include "notify_os.h"
#include "screen_ids.h"
#include "settings_store.h"
#include "shell_parts.h"
#include "theme.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define PULSE_TICK_MS 50
#define FLASH_TICK_MS 500 // re-checks the thermal watchdog and re-sends the levels (a C6 reset forgets them)

static char const TAG[] = "attention";

static lv_timer_t *pulse_timer; // LVGL task only
static uint32_t    pulse_start;
static atomic_bool flash_on;

bool flashlight_active(void) {
    return atomic_load(&flash_on);
}

static bool hot(void) {
    thermal_info_t t;
    thermal_info(&t);
    return t.level != TH_OK;
}

// ---- the pulse -------------------------------------------------------------------------------------------------------

static void pulse_end(void) {
    if (pulse_timer) {
        lv_timer_delete(pulse_timer);
        pulse_timer = NULL;
    }
    if (flashlight_active()) {
        return; // the flashlight owns the levels now
    }
    if (screen_idle_lit()) {
        backlight_resume(); // exactly the person's levels
    } else {
        backlight_temporary_keyboard(0); // the screen dimmed meanwhile: the dim stage has no keyboard light, its display level stays
    }
}

static void pulse_tick(lv_timer_t *timer) {
    (void)timer;
    int level = attn_keyboard_at(lv_tick_elaps(pulse_start));
    if (level < 0 || flashlight_active() || hot() || !screen_idle_lit()) {
        pulse_end();
    } else {
        backlight_temporary_keyboard((uint8_t)level);
    }
}

void attention_on_toast(uint32_t id, uint8_t level) {
    nr_entry_t e;
    if (pulse_timer || !lichen_notify_get(id, &e)) {
        return; // one pulse at a time; a burst of notices is one pulse
    }
    attn_state_t const state = {
        .dnd = lichen_notify_dnd(), .screen_lit = screen_idle_lit(), .hot = hot(), .flashlight = flashlight_active()};
    attn_mode_t mode = attn_mode_from_u8(settings_get_u8(ATTN_MODE_KEY, ATTN_MODE_DEFAULT));
    if (!attn_wants_pulse(mode, e.target, level, state)) {
        return;
    }
    pulse_start = lv_tick_get();
    pulse_timer = lv_timer_create(pulse_tick, PULSE_TICK_MS, NULL);
    if (pulse_timer) {
        backlight_temporary_keyboard((uint8_t)attn_keyboard_at(0));
    }
}

// ---- the flashlight ----------------------------------------------------------------------------------------------------

static lv_timer_t *flash_timer;

static void flash_apply(void) {
    attn_levels_t l = attn_flash_levels(hot());
    backlight_temporary(l.display, l.keyboard);
}

static void flash_tick(lv_timer_t *timer) {
    (void)timer;
    flash_apply();
}

static void on_flash_key(lv_event_t *e) {
    (void)e;
    shell_back();
}

static void on_flash_delete(lv_event_t *e) {
    (void)e;
    if (flash_timer) {
        lv_timer_delete(flash_timer);
        flash_timer = NULL;
    }
    atomic_store(&flash_on, false);
    backlight_resume(); // the person's levels, as they were
}

static lv_obj_t *build_flashlight(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *catcher = lv_obj_create(screen); // takes the focus, so any key reaches it
    lv_obj_remove_style_all(catcher);
    lv_obj_set_size(catcher, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(catcher, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(catcher, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    if (!lv_obj_get_group(catcher) && lv_group_get_default()) {
        lv_group_add_obj(lv_group_get_default(), catcher);
    }
    lv_obj_set_style_outline_width(catcher, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(catcher, 0, LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(catcher, on_flash_key, LV_EVENT_KEY, NULL);
    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "Any key turns it off");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x707070), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_event_cb(screen, on_flash_delete, LV_EVENT_DELETE, NULL);
    flash_timer = lv_timer_create(flash_tick, FLASH_TICK_MS, NULL);
    flash_apply();
    return screen;
}

void flashlight_open(void) {
    if (!flashlight_active()) {
        atomic_store(&flash_on, true); // before the build: a second call finds it taken; the idle clock holds the screen bright
        shell_open(build_flashlight, SCREEN_FLASHLIGHT);
        if (nav_current() != SCREEN_FLASHLIGHT) {
            atomic_store(&flash_on, false); // refused (a dialog is up): nothing will delete a screen
        }
    }
}

static void cmd_flashlight(char const *args) {
    (void)args;
    lichen_sys_ui_lock(0);
    flashlight_open();
    lichen_sys_ui_unlock();
    console_reply("flashlight: on, any key (fw keys send enter) turns it off");
}

void attention_init(void) {
    console_register("flashlight", "flashlight (white screen and keyboard light at full until a key; #164)", cmd_flashlight);
    ESP_LOGI(TAG, "message light: %s", attn_mode_name(attn_mode_from_u8(settings_get_u8(ATTN_MODE_KEY, ATTN_MODE_DEFAULT))));
}
