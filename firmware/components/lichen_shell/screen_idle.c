// Screen timeout (#43): after a while without a key the backlight dims (screen to its minimum, keyboard off), later
// both go off. Any key lights them again; a key that finds the screen off only wakes it. The keyboard is the activity
// source (its wake hook, ahead of the raw key hooks; "fw keys send" goes through it too, #375), plus a
// backlight change (fw bl, a slider). The remote key lock and an open test card (#355) hold it bright like an update.
// idle_policy.c decides; an LVGL timer applies the level, so the inhibits (update, first-run setup, Bench, the card)
// are read on the task that owns them.
// The clock screensaver takes the dim stage and the pocket lock engages with it or the dark screen (#75,
// screen_saver.c); a key that finds the saver up is swallowed like one that finds the screen off.
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "fw_console.h"
#include "idle_policy.h"
#include "lichen_sys_display.h"
#include "lichen_sys_keys.h"
#include "lvgl.h"
#include "settings_store.h"
#include "shell_parts.h"
#include "ui_remote.h"
#include "update_screen.h"

#include <stdio.h>
#include <stdlib.h>

#include <string.h>

#define TICK_MS 100 // how late a wake can light the screen

static char const TAG[] = "idle";

// Under lock: the keyboard task, the console and the LVGL task all touch these
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t      last_key_us;
static int64_t      card_touch_us; // a test card's last open or close (#355): what lifts the pocket lock, not a key
static idle_stage_t shown = {.level = IDLE_BRIGHT}; // what the swallow rule sees; a key sets it bright at once
static idle_times_t times;
static bool         saver_on = true; // Settings > Display (#75)
static bool         lock_on;
static bool         forced; // "fw screensaver test" until the next key
static bool         inhibited;
static bool         started;

static idle_level_t applied = IDLE_BRIGHT; // LVGL task: what the backlight was last set to
static uint32_t     seen_gen;              // LVGL task: lichen_sys_display_user_generation() at the last tick

static int64_t now_us(void) {
    return esp_timer_get_time();
}

bool screen_idle_lit(void) {
    portENTER_CRITICAL(&lock);
    bool lit = started && shown.level == IDLE_BRIGHT && !shown.saver;
    portEXIT_CRITICAL(&lock);
    return lit;
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
    screen_saver_reload();
    portENTER_CRITICAL(&lock);
    times       = t;
    saver_on    = saver;
    lock_on     = lock_;
    last_key_us = now_us(); // a new setting counts from now, not from the last key
    portEXIT_CRITICAL(&lock);
}

static void apply(idle_level_t level) {
    if (level == IDLE_OFF) {
        lichen_sys_display_idle(0, 0);
    } else if (level == IDLE_DIM) {
        lichen_sys_display_idle(LICHEN_SYS_DISPLAY_MIN, 0);
    } else {
        lichen_sys_display_resume(); // the live levels, a slider not yet saved included
    }
    ESP_LOGI(TAG, "backlight %s", idle_level_name(level));
    applied = level;
}

void screen_idle_touch(void) {
    portENTER_CRITICAL(&lock);
    last_key_us   = now_us();
    card_touch_us = last_key_us;
    forced        = false; // the card covers the screensaver test, as a key ends it
    portEXIT_CRITICAL(&lock);
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
    // the remote key lock (#190, #192): a script drives the UI, so no key of it may only wake the screen
    bool     busy      = must_show || bench_running() || flashlight_active() || ui_remote_locked();
    bool     card      = test_cards_open(); // LVGL task, as the cards themselves
    bool     setup     = setup_running();
    bool     home      = shell_home_shown(); // no cover over the boot screen or home's fade-in
    uint32_t gen   = lichen_sys_display_user_generation();
    bool     touch = gen != seen_gen; // fw bl or a slider lit the screen: that is activity too
    seen_gen       = gen;
    portENTER_CRITICAL(&lock);
    int64_t now = now_us();
    if (touch) {
        last_key_us = now;
    }
    uint32_t    idle_s = (uint32_t)((now - last_key_us) / 1000000);
    idle_hold_t hold   = idle_hold(busy, card, setup, idle_s);
    if (hold == IDLE_HOLD_RESTART) {
        last_key_us = now; // the full timeout once the update or the Bench run is over
        idle_s      = 0;
        forced      = false;
    }
    uint32_t     card_s = (uint32_t)((now - card_touch_us) / 1000000);
    idle_level_t level  = idle_level(idle_after_hold(card, idle_s, times), times, hold != IDLE_HOLD_NONE);
    idle_stage_t st    = idle_stage(level, saver_on, lock_on, forced && hold == IDLE_HOLD_NONE);
    if (!home) {
        st.saver = st.lock = false;
    }
    level     = st.level;
    shown     = st;
    inhibited = hold != IDLE_HOLD_NONE;
    portEXIT_CRITICAL(&lock);
    if (idle_lock_lifted(must_show, card, card_s)) {
        screen_lock_release(); // also drops the lock's key listener, so the card alone gets Esc
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

#ifdef CONFIG_LICHEN_DEV_COMMANDS
#define IDLE_USAGE "idle | idle test <seconds> (screen timeout state; fake the time since the last key)"
#else
#define IDLE_USAGE "idle (screen timeout state)" // no test arm in a release image (#421)
#endif

// "fw idle": the state; "fw idle test <s>": the idle clock as if the last key was s seconds ago (bench)
static void cmd_idle(char const *args) {
#ifdef CONFIG_LICHEN_DEV_COMMANDS // a debug arm (#421): not in a release image
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
#endif
    if (*args) {
        console_reply(IDLE_USAGE);
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
                  inhibit ? " (held bright: update, setup, Bench, key lock or test card)" : "");
}

void screen_idle_init(void) {
#ifdef CONFIG_LICHEN_DEV_COMMANDS
    console_register("idle", "idle | idle test <s> (screen timeout: state, or fake s seconds without a key)", cmd_idle);
#else
    console_register("idle", "idle (screen timeout: state)", cmd_idle);
#endif
}

void screen_idle_start(void) {
    if (started) {
        return;
    }
    screen_idle_reload();
    seen_gen = lichen_sys_display_user_generation();
    if (!lv_timer_create(tick, TICK_MS, NULL)) {
        ESP_LOGE(TAG, "no timer: no screen timeout");
        return;
    }
    portENTER_CRITICAL(&lock);
    started = true;
    portEXIT_CRITICAL(&lock);
    lichen_sys_keys_set_wake(screen_idle_key); // presses only: the keyboard drops the release of a swallowed one
}
