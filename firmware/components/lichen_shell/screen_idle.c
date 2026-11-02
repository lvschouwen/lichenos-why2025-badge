// Screen timeout (#43): after a while without a key the backlight dims (screen to its minimum, keyboard off), later
// both go off. Any key lights them again; a key that finds the screen off only wakes it. The keyboard is the activity
// source (its wake hook, ahead of the raw key hooks), plus "fw keys send" so the bench can wake the badge, plus a
// backlight change (fw bl, a slider). idle_policy.c decides; an
// LVGL timer applies the level, so the inhibits (update, first-run setup, Bench) are read on the task that owns them.
// The clock screensaver takes the dim stage and the pocket lock engages with it or the dark screen (#75,
// screen_saver.c); a key that finds the saver up is swallowed like one that finds the screen off.
#include "backlight.h"
#include "backlight_level.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "fw_console.h"
#include "idle_policy.h"
#include "keyboard.h"
#include "lvgl.h"
#include "settings_store.h"
#include "shell_parts.h"
#include "update_screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICK_MS 100 // how late a wake can light the screen

static char const TAG[] = "idle";

// Under lock: the keyboard task, the console and the LVGL task all touch these
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t      last_key_us;
static idle_stage_t shown = {.level = IDLE_BRIGHT}; // what the swallow rule sees; a key sets it bright at once
static idle_times_t times;
static bool         saver_on = true; // Settings > Display (#75)
static bool         lock_on;
static bool         forced; // "fw screensaver test" until the next key
static bool         inhibited;
static bool         started;

static idle_level_t applied = IDLE_BRIGHT; // LVGL task: what the backlight was last set to
static uint32_t     seen_gen;              // LVGL task: backlight_user_generation() at the last tick

static int64_t now_us(void) {
    return esp_timer_get_time();
}

bool screen_idle_key(void) {
    portENTER_CRITICAL(&lock);
    bool swallow = started && idle_key_swallowed(shown);
    last_key_us  = now_us();
    shown        = (idle_stage_t){.level = IDLE_BRIGHT};
    forced       = false;
    portEXIT_CRITICAL(&lock);
    return swallow;
}


void screen_idle_reload(void) {
    idle_times_t t = idle_times_from_10s(settings_get_u8("idle_dim", IDLE_DIM_DEFAULT_10S),
                                         settings_get_u8("idle_off", IDLE_OFF_DEFAULT_10S));
    bool saver = settings_get_u8(IDLE_SAVER_KEY, 1) != 0;
    bool lock_ = settings_get_u8(IDLE_LOCK_KEY, 0) != 0;
    portENTER_CRITICAL(&lock);
    times       = t;
    saver_on    = saver;
    lock_on     = lock_;
    last_key_us = now_us(); // a new setting counts from now, not from the last key
    portEXIT_CRITICAL(&lock);
}

static void apply(idle_level_t level) {
    if (level == IDLE_OFF) {
        backlight_idle(0, 0);
    } else if (level == IDLE_DIM) {
        backlight_idle(BACKLIGHT_DISPLAY_MIN, 0);
    } else {
        backlight_resume(); // the live levels, a slider not yet saved included
    }
    ESP_LOGI(TAG, "backlight %s", idle_level_name(level));
    applied = level;
}

void screen_idle_force_saver(void) {
    portENTER_CRITICAL(&lock);
    forced = true;
    portEXIT_CRITICAL(&lock);
}

char const *screen_idle_stage_name(void) {
    portENTER_CRITICAL(&lock);
    idle_stage_t st = shown;
    portEXIT_CRITICAL(&lock);
    return st.saver ? "screensaver" : idle_level_name(st.level);
}

static void tick(lv_timer_t *timer) {
    (void)timer;
    bool     must_show = update_screen_busy() || power_going_down(); // nothing may cover these (#75)
    bool     busy      = must_show || bench_running();
    bool     setup     = setup_running();
    bool     home      = shell_home_shown(); // no cover over the boot screen or home's fade-in
    uint32_t gen   = backlight_user_generation();
    bool     touch = gen != seen_gen; // fw bl or a slider lit the screen: that is activity too
    seen_gen       = gen;
    portENTER_CRITICAL(&lock);
    int64_t now = now_us();
    if (touch) {
        last_key_us = now;
    }
    uint32_t    idle_s = (uint32_t)((now - last_key_us) / 1000000);
    idle_hold_t hold   = idle_hold(busy, setup, idle_s);
    if (hold == IDLE_HOLD_RESTART) {
        last_key_us = now; // the full timeout once the update or the Bench run is over
        idle_s      = 0;
        forced      = false;
    }
    idle_level_t level = idle_level(idle_s, times, hold != IDLE_HOLD_NONE);
    idle_stage_t st    = idle_stage(level, saver_on, lock_on, forced && hold == IDLE_HOLD_NONE);
    if (!home) {
        st.saver = st.lock = false;
    }
    level     = st.level;
    shown     = st;
    inhibited = hold != IDLE_HOLD_NONE;
    portEXIT_CRITICAL(&lock);
    if (must_show) {
        screen_lock_release();
    } else if (st.lock) {
        screen_lock_engage();
    }
    screen_saver_show(st.saver);
    if (touch && level == IDLE_BRIGHT) {
        applied = IDLE_BRIGHT; // the change already lit it with the levels asked for
    }
    if (level != applied) {
        apply(level);
    }
}

static void times_text(char *out, size_t cap, idle_times_t t) {
    char dim[16], off[16];
    idle_choice_text((uint8_t)(t.dim_s / 10), dim, sizeof(dim));
    idle_choice_text((uint8_t)(t.off_s / 10), off, sizeof(off));
    snprintf(out, cap, "dim after %s, off after %s", dim, off);
}

// "fw idle": the state; "fw idle test <s>": the idle clock as if the last key was s seconds ago (bench)
static void cmd_idle(char const *args) {
    if (strncmp(args, "test ", 5) == 0) {
        char         *end;
        unsigned long s = strtoul(args + 5, &end, 10);
        if (end == args + 5 || *end || s > 86400) {
            console_reply("idle test <seconds 0-86400>");
            return;
        }
        portENTER_CRITICAL(&lock);
        last_key_us = now_us() - (int64_t)s * 1000000;
        portEXIT_CRITICAL(&lock);
        console_reply("idle clock set to %lu s (applied within %u ms)", s, TICK_MS);
        return;
    }
    if (*args) {
        console_reply("idle | idle test <seconds> (screen timeout state; fake the time since the last key)");
        return;
    }
    portENTER_CRITICAL(&lock);
    bool         on      = started;
    bool         inhibit = inhibited;
    idle_times_t t       = times;
    uint32_t     idle_s  = (uint32_t)((now_us() - last_key_us) / 1000000);
    portEXIT_CRITICAL(&lock);
    char what[64];
    times_text(what, sizeof(what), t);
    if (!on) {
        console_reply("idle: not running (no keyboard), %s", what);
        return;
    }
    console_reply("idle: %s, %lu s since the last key, %s%s", screen_idle_stage_name(), (unsigned long)idle_s, what,
                  inhibit ? " (held bright: update, setup or Bench)" : "");
}

void screen_idle_init(void) {
    console_register("idle", "idle | idle test <s> (screen timeout: state, or fake s seconds without a key)", cmd_idle);
}

void screen_idle_start(void) {
    if (started) {
        return;
    }
    screen_idle_reload();
    seen_gen = backlight_user_generation();
    if (!lv_timer_create(tick, TICK_MS, NULL)) {
        ESP_LOGE(TAG, "no timer: no screen timeout");
        return;
    }
    portENTER_CRITICAL(&lock);
    started = true;
    portEXIT_CRITICAL(&lock);
    keyboard_set_wake_hook(screen_idle_key); // presses only: the keyboard drops the release of a swallowed one
}
