// "fw ui lock" and "fw ui open" (#190). The lock state is shared by the keyboard task (every physical key), the console
// task (commands) and the LVGL task (the timer that ends it and draws the banner), so it lives under a spinlock; the
// rules themselves are remote_ctl.c. It is RAM only: a restart always ends it.
#include "ui_remote.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "fw_console.h"
#include "key_cause.h"
#include "key_matrix.h"
#include "lichen_app.h"
#include "lichen_sys_app.h"
#include "lichen_sys_keys.h"
#include "lichen_sys_ui.h"
#include "nav.h"
#include "raw_keys.h"
#include "remote_ctl.h"
#include "shell_parts.h"
#include "update_screen.h"

#include <stdio.h>

#include <string.h>

static char const TAG[] = "ui_remote";

#define TICK_MS 100 // the Esc hold and the timeout are judged this often (and on every key)

static rlock_t      lock;
static portMUX_TYPE lock_mux = portMUX_INITIALIZER_UNLOCKED;
static lv_timer_t  *timer;  // LVGL task; runs while the lock or its banner is up
static bool         banner; // what the status bar shows (LVGL task)

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Keyboard task, every physical key before anything else sees it
static bool gate(uint8_t code, bool pressed) {
    portENTER_CRITICAL(&lock_mux);
    bool drop = rlock_key(&lock, code, code == KEY_MATRIX_ESC_CODE, pressed, now_ms());
    portEXIT_CRITICAL(&lock_mux);
    return drop;
}

// Keyboard task: the chip was reset, every key is up
static void gate_reset(void) {
    portENTER_CRITICAL(&lock_mux);
    rlock_keys_reset(&lock);
    portEXIT_CRITICAL(&lock_mux);
}

// Console task, at the start and the end of every command: the timeout counts from the last one
static void console_activity(void) {
    portENTER_CRITICAL(&lock_mux);
    rlock_touch(&lock, now_ms());
    portEXIT_CRITICAL(&lock_mux);
}

bool ui_remote_locked(void) {
    portENTER_CRITICAL(&lock_mux);
    bool on = rlock_tick(&lock, now_ms());
    portEXIT_CRITICAL(&lock_mux);
    return on;
}

static void tick(lv_timer_t *t) {
    portENTER_CRITICAL(&lock_mux);
    bool        on  = rlock_tick(&lock, now_ms());
    rlock_end_t end = rlock_take_end(&lock);
    portEXIT_CRITICAL(&lock_mux);
    if (end != RLOCK_END_NONE) {
        ESP_LOGI(TAG, "key lock off (%s)", rlock_end_name(end));
    }
    if (end == RLOCK_END_ESC) {
        screen_idle_key(); // the holder took the keys back: light the screen as a key would (it may have dimmed)
    }
    if (on != banner) {
        banner = on;
        status_bar_remote_lock(on);
    }
    if (!on) {
        lv_timer_pause(t);
    }
}

void ui_remote_start(void) {
    lichen_sys_keys_set_gate(gate, gate_reset);
    console_set_activity(console_activity);
}

void ui_remote_report(void) {
    portENTER_CRITICAL(&lock_mux);
    uint32_t now  = now_ms();
    bool     on   = rlock_tick(&lock, now);
    uint32_t left = rlock_left_s(&lock, now);
    portEXIT_CRITICAL(&lock_mux);
    if (on) {
        console_reply("ui lock: on, %u s left", (unsigned)left);
    } else {
        console_reply("ui lock: off");
    }
}

// The banner and the timer follow the state at once, not at the next tick
static void kick_timer(void) {
    lichen_sys_ui_lock(0);
    if (!timer) {
        timer = lv_timer_create(tick, TICK_MS, NULL);
    }
    if (timer) {
        lv_timer_resume(timer);
        tick(timer);
    }
    lichen_sys_ui_unlock();
}

void ui_remote_hold(bool on) {
    portENTER_CRITICAL(&lock_mux);
    if (on) {
        rlock_on(&lock, RLOCK_MAX_S, now_ms());
    } else {
        rlock_off(&lock);
    }
    portEXIT_CRITICAL(&lock_mux);
    kick_timer();
}

void ui_remote_touch(void) {
    console_activity();
}

void ui_remote_lock_cmd(char const *args) {
    uint32_t     seconds;
    rlock_args_t what = rlock_parse(args, &seconds);
    if (what == RLOCK_ARGS_ERR) {
        console_reply("ui lock: usage: ui lock [on [1-%u] | off]", (unsigned)RLOCK_MAX_S);
        return;
    }
    if (what == RLOCK_ARGS_SHOW) {
        ui_remote_report();
        return;
    }
    portENTER_CRITICAL(&lock_mux);
    if (what == RLOCK_ARGS_ON) {
        rlock_on(&lock, seconds, now_ms());
    } else {
        rlock_off(&lock);
    }
    portEXIT_CRITICAL(&lock_mux);
    kick_timer();
    if (what == RLOCK_ARGS_ON) {
        console_reply("ui lock: on, %u s after the last command (Esc held %u s ends it)", (unsigned)seconds,
                      (unsigned)(RLOCK_ESC_HOLD_MS / 1000));
    } else {
        console_reply("ui lock: off");
    }
}

// Why the shell should not open a screen now, NULL when it may. LVGL lock held.
static char const *open_refused(void) {
    if (!shell_home_shown()) {
        return "still booting";
    }
    if (shell_dialog_group()) {
        return "a dialog is open"; // the careful confirms too: the person answers it, not a script
    }
    if (screen_lock_active()) {
        return "the screen is locked";
    }
    if (update_screen_busy() || power_going_down()) {
        return "an update, restart or power off is under way";
    }
    if (setup_running()) {
        return "the first-run setup is on screen";
    }
    if (raw_keys_test_open()) {
        return "the Keyboard test is open"; // it owns the keys, like the shape-key shortcuts see it
    }
    return NULL;
}

static void list_ids(void) {
    char   ids[160] = "";
    size_t used     = 0;
    for (size_t i = 0; i < lichen_app_count() && used < sizeof(ids); i++) {
        used += (size_t)snprintf(ids + used, sizeof(ids) - used, "%s%s", i ? " " : "", lichen_app_at(i)->id);
    }
    console_reply("ui open: apps: home %s", ids);
}

static int find_app(char const *arg) {
    char const *ids[LICHEN_MAX_APPS], *names[LICHEN_MAX_APPS];
    size_t      n = lichen_app_count();
    for (size_t i = 0; i < n && i < LICHEN_MAX_APPS; i++) {
        ids[i]   = lichen_app_at(i)->id;
        names[i] = lichen_app_at(i)->name;
    }
    return rctl_find_app(arg, ids, names, n < LICHEN_MAX_APPS ? n : LICHEN_MAX_APPS);
}

// From home, like a tile's Enter and the shape-key shortcuts (shortcuts.c open_from_home): Esc then leads home
void ui_remote_open_cmd(char const *args) {
    char const *home_words[] = {"home"};
    bool        to_home      = rctl_find_app(args, home_words, home_words, 1) == 0;
    int         index        = to_home ? -1 : find_app(args);
    if (!to_home && index < 0) {
        console_reply("ui open: no app '%s'", args + strspn(args, " "));
        list_ids();
        return;
    }
    lichen_sys_ui_lock(0);
    char const *why = open_refused();
    bool        ok  = false;
    if (!why) {
        shell_home();
        if (!to_home) {
            // An operator's command, not a key (#400): opening a stopped MeshCore here starts it, as from the tile
            key_cause_t previous = key_cause_enter(KEY_CAUSE_CONSOLE);
            shell_open_app((size_t)index);
            key_cause_leave(previous);
        }
        ok = to_home ? nav_depth() == 1 : nav_current() == 1 + index;
    }
    lichen_sys_ui_unlock();
    char const *name = to_home ? "home" : lichen_app_at((size_t)index)->name;
    if (why) {
        console_reply("ui open: not now (%s)", why);
    } else if (!ok) {
        console_reply("ui open: %s did not open", name);
    } else {
        console_reply("ui open: %s", name);
    }
}
