// The Lichen shell (#66): boot screen, then home with the status bar over it, and a stack of screens that Esc walks
// back down. nav.c (pure, host-tested) holds which screen is where; this file keeps the LVGL screens in step with it.
#include "shell.h"

#include "esp_log.h"
#include "fw_console.h"
#include "esp_lvgl_port.h"
#include "key_matrix.h"
#include "lichen_app.h"
#include "nav.h"
#include "raw_keys.h"
#include "scroll_anchor.h"
#include "settings_store.h"
#include "shell_parts.h"
#include "fonts.h"
#include "text_entry.h"
#include "theme.h"
#include "update_screen.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static char const TAG[] = "shell";


typedef struct {
    lv_obj_t   *screen;
    lv_group_t *group;
    bool      (*on_esc)(void *); // shell_on_esc(): true when it used the Esc
    void       *on_esc_arg;
} frame_t;

shell_sources_t shell_sources;

static bool                 up;
static lv_indev_t          *keys;
static frame_t              frames[NAV_MAX_DEPTH]; // frames[0] is home once boot is over
static lv_obj_t            *status_bar;

void shell_set_sources(shell_sources_t const *sources) {
    shell_sources = *sources;
}

bool shell_up(void) {
    return up;
}

void shell_style_screen(lv_obj_t *screen) {
    lv_obj_set_style_bg_color(screen, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_font(screen, lichen_font(LICHEN_FONT_SMALL), 0);
}

// An open dialog keeps the keys whatever opens or closes below it (#14 T-B)
static void focus_keys(void) {
    if (!keys) {
        return;
    }
    if (shell_dialog_group()) {
        lv_indev_set_group(keys, shell_dialog_group());
    } else if (nav_depth() > 0 && frames[nav_depth() - 1].group) {
        lv_indev_set_group(keys, frames[nav_depth() - 1].group);
    } else if (boot_screen_group()) {
        lv_indev_set_group(keys, boot_screen_group());
    }
}

// The dialog opened or closed (#14 T-B): a key still held (the Enter that answered it) must not be released into the
// other group, where its release would click a widget that never saw the press
void shell_refocus(void) {
    if (keys) {
        lv_indev_wait_release(keys);
    }
    focus_keys();
}

static void show_top(void) {
    lv_screen_load(frames[nav_depth() - 1].screen);
    focus_keys();
}

static void drop(frame_t *frame) {
    lv_group_delete(frame->group);
    lv_obj_delete(frame->screen);
    frame->group  = NULL;
    frame->screen = NULL;
}

// Runs from lv_async_call, outside the indev's key handling, so deleting the screen that had the key is safe.
// Note: LVGL 9 runs async calls queued together newest first, so a pop and its follow-up must be ONE async step
// (back_job below), never two lv_async_call()s expected to run in order.
static bool pop_one(void) {
    int depth = nav_depth();
    if (depth <= 1 || !frames[0].screen || update_screen_blocks_back()) {
        return false; // at home (or still booting): Esc does nothing
    }
    nav_back();
    show_top();
    drop(&frames[depth - 1]);
    return true;
}

static void go_back(void *arg) {
    (void)arg;
    pop_one();
}

static bool   (*building_esc)(void *); // set by shell_on_esc() while shell_open() runs a builder
static void    *building_esc_arg;

void shell_on_esc(bool (*fn)(void *arg), void *arg) {
    building_esc     = fn;
    building_esc_arg = arg;
}

// The top screen may use the Esc first (clear a search or a draft); if not, it goes back
void shell_esc(void) {
    if (shell_dialog_group()) {
        return; // the dialog's own Esc (#14 T-B)
    }
    int depth = nav_depth();
    frame_t *top = depth > 0 ? &frames[depth - 1] : NULL;
    if (top && top->on_esc && top->on_esc(top->on_esc_arg)) {
        return;
    }
    lv_async_call(go_back, NULL);
}

void shell_back(void) {
    lv_async_call(go_back, NULL);
}

typedef struct {
    lv_obj_t *target; // pop down to this screen; NULL = pop one
    void (*then)(void *);
    void *arg;
} back_job_t;

static bool in_stack(lv_obj_t *screen) {
    for (int i = 0; i < nav_depth(); i++) {
        if (frames[i].screen == screen) {
            return true;
        }
    }
    return false;
}

static void back_step(void *data) {
    back_job_t *job = data;
    if (!job->target) {
        pop_one();
    } else if (in_stack(job->target)) { // a screen that is gone is not popped towards
        while (frames[nav_depth() - 1].screen != job->target && pop_one()) {
        }
    }
    if (job->then) {
        job->then(job->arg); // the target is showing now
    }
    lv_free(job);
}

static void back_later(lv_obj_t *target, void (*then)(void *), void *arg) {
    back_job_t *job = lv_malloc(sizeof(*job));
    if (!job) {
        return;
    }
    *job = (back_job_t){target, then, arg};
    lv_async_call(back_step, job);
}

void shell_back_then(void (*then)(void *), void *arg) {
    back_later(NULL, then, arg);
}

void shell_back_to(lv_obj_t *screen, void (*then)(void *), void *arg) {
    back_later(screen, then, arg);
}

lv_group_t *shell_top_group(void) {
    return frames[nav_depth() - 1].group;
}

static bool log_keys; // "fw keys on": a diagnostic, not saved

// Typed passwords must not reach the log (readable over Wi-Fi with the token): keys are not logged while a secret text
// area has the focus, also after Tab made its text visible (so the password mode is not the test)
static bool password_focused(void) {
    lv_group_t *g  = lv_indev_get_group(keys);
    lv_obj_t   *ta = g ? lv_group_get_focused(g) : NULL;
    return ta && (lv_obj_has_flag(ta, TEXT_ENTRY_SECRET_FLAG) ||
                  (lv_obj_check_type(ta, &lv_textarea_class) && lv_textarea_get_password_mode(ta)));
}

static void on_key(lv_event_t *e) {
    (void)e;
    uint32_t key     = lv_indev_get_key(keys);
    bool     pressed = lv_indev_get_state(keys) == LV_INDEV_STATE_PRESSED;
    if (log_keys && !password_focused()) {
        ESP_LOGW(TAG, "key %lu (0x%02lx) %s", (unsigned long)key, (unsigned long)key, pressed ? "pressed" : "released");
    }
    if (key == LV_KEY_ESC && pressed) {
        shell_esc();
    }
}

// 250 ms fade plus a little: a screen opened while the boot screen is still being dropped would race it
#define HOME_FADE_MS 250
#define FADE_SETTLE_MS 300
static uint32_t fade_started;
static bool     fading;

static bool home_settling(void) {
    if (fading && lv_tick_elaps(fade_started) >= FADE_SETTLE_MS) {
        fading = false;
    }
    return fading;
}

bool shell_home_shown(void) {
    bool shown;
    lvgl_port_lock(0);
    shown = frames[0].screen && !home_settling();
    lvgl_port_unlock();
    return shown;
}

static void new_home_frame(frame_t *frame) {
    frame->screen = home_create();
    frame->group  = lv_group_create();
    lv_group_add_obj(frame->group, frame->screen);
}

static void first_run_cb(lv_timer_t *timer) {
    (void)timer;
    setup_start();
}

void shell_home(void) {
    lvgl_port_lock(0);
    if (frames[0].screen) {
        // Home first, then the screens it replaces: never delete the one that is showing
        int depth = nav_depth();
        while (nav_depth() > 1) {
            nav_back();
        }
        show_top();
        for (int i = 1; i < depth; i++) {
            drop(&frames[i]);
        }
        lvgl_port_unlock();
        return;
    }
    nav_reset();
    new_home_frame(&frames[0]);
    if (!status_bar) {
        status_bar = status_bar_create(lv_layer_top());
    }
    // The boot screen goes once home has faded in
    lv_screen_load_anim(frames[0].screen, LV_SCREEN_LOAD_ANIM_FADE_IN, HOME_FADE_MS, 0, true);
    fade_started = lv_tick_get();
    fading       = true;
    focus_keys();
    // First run: the welcome opens once home has settled (shell_open refuses while it fades)
    lv_timer_t *first = lv_timer_create(first_run_cb, FADE_SETTLE_MS + 50, NULL);
    lv_timer_set_repeat_count(first, 1);
    lvgl_port_unlock();
}

// The app list or the layout changed: a fresh home, and back to it from wherever the badge is
void shell_rebuild_home(void) {
    lvgl_port_lock(0);
    if (!frames[0].screen || home_settling()) {
        lvgl_port_unlock();
        return;
    }
    frame_t old[NAV_MAX_DEPTH];
    int     depth = nav_depth();
    memcpy(old, frames, sizeof(old));
    memset(&frames[1], 0, sizeof(frames) - sizeof(frames[0]));
    nav_reset();
    new_home_frame(&frames[0]);
    show_top();
    for (int i = 0; i < depth; i++) {
        drop(&old[i]);
    }
    lvgl_port_unlock();
}

void shell_scroll_tame(lv_obj_t *obj) {
    lv_obj_set_scroll_elastic(obj, false);
    lv_obj_set_scroll_momentum(obj, false);
}

void shell_scroll_by(lv_obj_t *obj, int32_t dy) {
    // From where a running animation is heading, not from where it is drawn (a burst of keys would lose steps, #34);
    // clamped to the content: content that fits has nothing to scroll
    lv_point_t end;
    lv_obj_update_layout(obj);
    lv_obj_get_scroll_end(obj, &end);
    int32_t max_y = lv_obj_get_scroll_y(obj) + lv_obj_get_scroll_bottom(obj);
    lv_obj_scroll_to_y(obj, scroll_step_target(end.y, dy, max_y), LV_ANIM_ON);
}

static lichen_app_t const *opening; // for build_app, which shell_open() calls without arguments

static lv_obj_t *build_app(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    shell_style_screen(screen);
    lv_obj_set_style_pad_top(screen, THEME_STATUS_BAR_H + THEME_PAD, 0);
    lv_obj_set_style_pad_hor(screen, THEME_PAD, 0);
    shell_scroll_tame(screen);
    opening->open(screen);
    return screen;
}

void shell_open_app(size_t index) {
    lichen_app_t const *app = lichen_app_at(index);
    if (!app || !app->open) {
        return;
    }
    lvgl_port_lock(0);
    opening = app;
    shell_open(build_app, 1 + (int)index);
    lvgl_port_unlock();
}

void shell_open(lv_obj_t *(*build)(void), int screen_id) {
    lvgl_port_lock(0);
    if (!frames[0].screen || home_settling() || !nav_push(screen_id)) {
        ESP_LOGW(TAG, "screen %d not opened (%s)", screen_id,
                 !frames[0].screen || fading ? "still booting" : "stack full or already open");
        lvgl_port_unlock();
        return;
    }
    lv_group_t *group    = lv_group_create();
    lv_group_t *previous = lv_group_get_default();
    lv_group_set_default(group);
    shell_on_esc(NULL, NULL);
    lv_obj_t *screen = build();
    bool (*on_esc)(void *) = building_esc;
    void *on_esc_arg       = building_esc_arg;
    shell_on_esc(NULL, NULL);
    lv_group_set_default(previous);
    if (!screen) {
        nav_back();
        lv_group_delete(group);
        lvgl_port_unlock();
        return;
    }
    if (lv_group_get_obj_count(group) == 0) {
        lv_group_add_obj(group, screen); // keys still reach a screen without focusable widgets
    }
    frames[nav_depth() - 1] = (frame_t){screen, group, on_esc, on_esc_arg};
    show_top();
    lvgl_port_unlock();
}

void shell_close_all_but_top(void) {
    lvgl_port_lock(0);
    int depth = nav_depth();
    if (depth <= 1 || !frames[0].screen) {
        lvgl_port_unlock();
        return;
    }
    // The top screen is showing: delete the others only once it has its own slot, so none is deleted twice
    frame_t old[NAV_MAX_DEPTH];
    int     top_id = nav_current();
    frame_t top    = frames[depth - 1];
    memcpy(old, frames, sizeof(old));
    memset(frames, 0, sizeof(frames));
    nav_reset();
    nav_push(top_id);
    frames[1] = top;
    show_top();
    for (int i = 0; i < depth - 1; i++) {
        drop(&old[i]);
    }
    lvgl_port_unlock();
}

void shell_attach_keys(lv_indev_t *indev) {
    if (!indev) {
        return;
    }
    lvgl_port_lock(0);
    keys = indev;
    lv_indev_add_event_cb(keys, on_key, LV_EVENT_KEY, NULL);
    screen_idle_start(); // no keyboard, no timeout: nothing could wake the screen
    if (frames[0].screen) {
        focus_keys();
    } else if (boot_screen_group()) {
        lv_indev_set_group(keys, boot_screen_group()); // any key shows the boot details
    }
    lvgl_port_unlock();
}

// "fw keys log on": raw key events (before the keyboard maps it, so shape keys and Fn show too), only those captured while
// the Keyboard Test was open: anywhere else the codes could spell a password. Not persisted.
static bool log_raw;

static void on_raw_key(key_raw_event_t const *ev, void *arg) {
    (void)arg;
    if (!ev->test_open) {
        return;
    }
    char text[64];
    key_matrix_format(text, sizeof(text), ev);
    ESP_LOGW(TAG, "raw key %s", text);
}

static void set_raw_log(bool on) {
    lvgl_port_lock(0);
    if (on && !log_raw) {
        log_raw = raw_keys_listen(on_raw_key, NULL, false);
    } else if (!on && log_raw) {
        raw_keys_unlisten(on_raw_key, NULL);
        log_raw = false;
    }
    lvgl_port_unlock();
}

static void cmd_keys(char const *args) {
    if (strncmp(args, "log ", 4) == 0 && (strcmp(args + 4, "on") == 0 || strcmp(args + 4, "off") == 0)) {
        set_raw_log(args[5] == 'n');
        console_reply("raw keys %s", log_raw ? "logged" : "not logged");
        return;
    }
    if (strncmp(args, "send ", 5) == 0) {
        ui_probe_send(args + 5);
        return;
    }
    if (strcmp(args, "on") == 0 || strcmp(args, "off") == 0) {
        log_keys = args[1] == 'n';
    } else if (*args) {
        console_reply("keys on|off (log every key the screen receives)");
        return;
    }
    console_reply("keys %s%s", log_keys ? "logged" : "not logged", keys ? "" : " (no keyboard attached)");
}

void shell_start(lv_display_t *display) {
    home_init();
    setup_init();
    screen_idle_init();
    console_register("keys", "keys on|off (log every key) | keys log on|off (log raw key events) | keys send <up down left right enter esc tab bksp wait text:..|>", cmd_keys);
    ui_probe_init();
    ui_check_start();
    ui_perf_start();
    if (!getenv("TZ")) {
        settings_apply_tz(); // main.c normally did this already
    }
    settings_app_register();
    about_app_register();
    keytest_app_register();
    bench_app_register();
    crashlog_app_register();
    notify_app_register();
    lvgl_port_lock(0);
    lv_display_set_default(display);
    lv_obj_t *boot = boot_screen_create();
    lv_screen_load_anim(boot, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true); // drops LVGL's empty start screen
    update_screen_init();
    notify_ui_start();
    up = true;
    lvgl_port_unlock();
    sys_notices_start();
}
