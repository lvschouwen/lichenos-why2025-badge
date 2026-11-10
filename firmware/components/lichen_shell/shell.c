// The Lichen shell (#66): boot screen, then home with the status bar over it, and a stack of screens that Esc walks
// back down. nav.c (pure, host-tested) holds which screen is where; this file keeps the LVGL screens in step with it.
#include "shell.h"

#include "esp_log.h"
#include "fw_console.h"
#include "lichen_sys_ui.h"
#include "lichen_sys_mem.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "key_matrix.h"
#include "lichen_app.h"
#include "lichen_radio.h"
#include "nav.h"
#include "notes_app.h"
#include "raw_keys.h"
#include "restart_frame.h"
#include "scroll_anchor.h"
#include "settings_store.h"
#include "shell_parts.h"
#include "stress_workers.h"
#include "ui_remote.h"
#include "fonts.h"
#include "text_entry.h"
#include "theme.h"
#include "update_screen.h"
#include "shell_ui_ops.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static char const TAG[] = "shell";


typedef struct {
    lv_obj_t   *screen;
    lv_group_t *group;
    bool      (*on_esc)(void *); // shell_on_esc(): true when it used the Esc
    void       *on_esc_arg;
    void      (*on_search)(void *); // shell_on_search() (#79)
    void       *on_search_arg;
} frame_t;

shell_sources_t shell_sources;

static bool                 up;
static lv_indev_t          *keys;
static frame_t              frames[NAV_MAX_DEPTH]; // frames[0] is home once boot is over
static lv_obj_t            *status_bar;
static lv_obj_t            *cover; // the screensaver or the lock screen (#75): shown instead of the top, not in the stack

void shell_set_sources(shell_sources_t const *sources) {
    shell_sources = *sources;
}

lv_obj_t *shell_status_bar(void) {
    return status_bar;
}

bool shell_up(void) {
    return up;
}

void shell_style_screen_impl(lv_obj_t *screen) {
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

// A cover stays in front whatever opens or closes below it; the keys still go to the top screen's group (the lock
// swallows them before LVGL, the saver's waking key never gets there)
static void show_top(void) {
    lv_screen_load(cover ? cover : frames[nav_depth() - 1].screen);
    focus_keys();
}

void shell_set_cover(lv_obj_t *screen, bool status_bar_shown) {
    cover = screen;
    if (status_bar) {
        lv_obj_set_hidden(status_bar, screen && !status_bar_shown);
    }
    if (frames[0].screen && nav_depth() > 0) {
        show_top();
    }
}

static void drop(frame_t *frame) {
    lv_group_delete(frame->group);
    lv_obj_delete(frame->screen);
    frame->group  = NULL;
    frame->screen = NULL;
}

// Runs as an async call (lv_async_call or the UI service's lichen_sys_ui_call), outside the indev's key handling, so
// deleting the screen that had the key is safe. Note: UI-service calls run FIFO among themselves, but a raw
// lv_async_call (shell_esc) runs newest first and the two do not order against each other (#225), so a pop and its
// follow-up stay ONE async step (back_job below), never two calls expected to run in order.
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

void shell_on_esc_impl(bool (*fn)(void *arg), void *arg) {
    building_esc     = fn;
    building_esc_arg = arg;
}

static void  (*building_search)(void *); // shell_on_search(), the same way
static void   *building_search_arg;

void shell_on_search_impl(void (*fn)(void *arg), void *arg) {
    building_search     = fn;
    building_search_arg = arg;
}

bool shell_search(void) {
    int      depth = nav_depth();
    frame_t *top   = depth > 0 ? &frames[depth - 1] : NULL;
    if (!top || !top->on_search) {
        return false;
    }
    top->on_search(top->on_search_arg);
    return true;
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

// shell_back and the app API's shell_back_then / shell_back_to (lichen_ui.h) may be called from a task other than LVGL's,
// and lv_malloc / lv_async_call are not thread safe: they go through the UI service, which takes the (recursive) LVGL
// lock (#118, #225)
void shell_back_impl(void) {
    lichen_sys_ui_call(go_back, NULL, 0);
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

static bool back_later(lv_obj_t *target, void (*then)(void *), void *arg) {
    lichen_sys_ui_lock(0);
    back_job_t *job    = lv_malloc(sizeof(*job));
    bool        queued = false;
    if (job) {
        *job   = (back_job_t){target, then, arg};
        queued = lichen_sys_ui_call(back_step, job, 0); // the lock is held: no wait
        if (!queued) {
            lv_free(job);
        }
    }
    lichen_sys_ui_unlock();
    return queued;
}

bool shell_back_then_impl(void (*then)(void *), void *arg) {
    return back_later(NULL, then, arg);
}

bool shell_back_to_impl(lv_obj_t *screen, void (*then)(void *), void *arg) {
    return back_later(screen, then, arg);
}

lv_obj_t *shell_stack_screen_impl(int from_top) {
    lichen_sys_ui_lock(0);
    int       at     = nav_depth() - 1 - from_top;
    lv_obj_t *screen = from_top >= 0 && at >= 0 ? frames[at].screen : NULL;
    lichen_sys_ui_unlock();
    return screen;
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
        ESP_LOGI(TAG, "key %lu (0x%02lx) %s", (unsigned long)key, (unsigned long)key, pressed ? "pressed" : "released");
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
    lichen_sys_ui_lock(0);
    shown = frames[0].screen && !home_settling();
    lichen_sys_ui_unlock();
    return shown;
}

static void new_home_frame(frame_t *frame) {
    frame->screen = home_create();
    frame->group  = lv_group_create();
    lv_group_add_obj(frame->group, frame->screen);
    frame->on_esc     = home_arrange_esc; // arranging is a state of home, not a screen of its own (#189)
    frame->on_esc_arg = frame->screen;
}

static void first_run_cb(lv_timer_t *timer) {
    (void)timer;
    setup_start();
}

void shell_home_impl(void) {
    lichen_sys_ui_lock(0);
    if (frames[0].screen) {
        // Home first, then the screens it replaces: never delete the one that is showing
        int depth = nav_depth();
        while (nav_depth() > 1) {
            nav_back();
        }
        home_arrange(frames[0].screen, false); // the home key also ends arranging (#189)
        show_top();
        for (int i = 1; i < depth; i++) {
            drop(&frames[i]);
        }
        lichen_sys_ui_unlock();
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
    lichen_sys_ui_unlock();
}

// The app list or the layout changed: a fresh home, and back to it from wherever the badge is
void shell_rebuild_home(void) {
    lichen_sys_ui_lock(0);
    if (!frames[0].screen || home_settling()) {
        lichen_sys_ui_unlock();
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
    lichen_sys_ui_unlock();
}

// The layout changed under the screens on top (Settings > Display, #128): home is rebuilt in its own frame and the
// stack stays, so Esc goes back to Settings, not home. Home itself showing is swapped like shell_rebuild_home.
void shell_rebuild_home_below(void) {
    lichen_sys_ui_lock(0);
    if (!frames[0].screen || home_settling()) {
        lichen_sys_ui_unlock();
        return;
    }
    frame_t old = frames[0];
    new_home_frame(&frames[0]);
    if (nav_depth() <= 1) {
        show_top(); // never delete the screen that is showing
    }
    drop(&old);
    lichen_sys_ui_unlock();
}

// Settings > Display > Arrange home (#189): every screen closes and home starts arranging; rebuilt first when the layout
// changed on that page (its own deferred rebuild would replace the arranging home)
void shell_arrange_home(bool rebuild) {
    lichen_sys_ui_lock(0);
    if (rebuild) {
        shell_rebuild_home();
    } else {
        shell_home();
    }
    if (frames[0].screen && nav_depth() == 1 && !home_settling()) {
        home_arrange(frames[0].screen, true);
    }
    lichen_sys_ui_unlock();
}

void shell_scroll_tame_impl(lv_obj_t *obj) {
    lv_obj_set_scroll_elastic(obj, false);
    lv_obj_set_scroll_momentum(obj, false);
}

void shell_scroll_by_impl(lv_obj_t *obj, int32_t dy) {
    // From where a running animation is heading, not from where it is drawn (a burst of keys would lose steps, #34);
    // clamped to the content: content that fits has nothing to scroll
    lv_point_t end;
    lv_obj_update_layout(obj);
    lv_obj_get_scroll_end(obj, &end);
    int32_t max_y = lv_obj_get_scroll_y(obj) + lv_obj_get_scroll_bottom(obj);
    lv_obj_scroll_to_y(obj, scroll_step_target(end.y, dy, max_y), LV_ANIM_ON);
}

// LVGL scrolls a focused scrollable object by a quarter of its height on every arrow (Left/Right too, with no
// animation) before the object's own key handler runs, and that jump kills shell_scroll_by's animation (#34). Every
// shell page that takes the keys moves its own selection or scrolls itself, so none keeps LVGL's arrow scrolling
// (#124, #128)
void shell_no_arrow_scroll_impl(lv_obj_t *obj) {
    lv_obj_set_scroll_with_arrow(obj, false);
}

static lichen_app_t const *opening; // for build_app, which shell_open() calls without arguments

static lv_obj_t *build_app(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    shell_style_screen(screen);
    lv_obj_set_style_pad_top(screen, THEME_STATUS_BAR_H + THEME_PAD, 0);
    lv_obj_set_style_pad_hor(screen, THEME_PAD, 0);
    shell_scroll_tame(screen);
    shell_no_arrow_scroll(screen);
    opening->open(screen);
    return screen;
}

void shell_open_app(size_t index) {
    lichen_app_t const *app = lichen_app_at(index);
    if (!app || !app->open) {
        return;
    }
    lichen_sys_ui_lock(0);
    opening = app;
    shell_open(build_app, 1 + (int)index);
    if (nav_current() == 1 + (int)index) {
        shortcuts_app_opened(index); // ✕ goes back here later (#79)
    }
    lichen_sys_ui_unlock();
}

static unsigned reused_count; // shell_open calls answered by a screen that was already open (#148)

unsigned shell_open_reuse_count_impl(void) {
    return reused_count;
}

bool shell_screen_open_impl(int screen_id, uint32_t key) {
    int frame;
    lichen_sys_ui_lock(0);
    bool open = nav_decide(screen_id, key, &frame) != NAV_OPEN_NEW;
    lichen_sys_ui_unlock();
    return open;
}

void shell_open_count_reuse_impl(void) {
    reused_count++;
}

// (id, key) is already on the stack (#148): nothing for the top one, else go back to it instead of building a second
// copy. Refused (and not counted as shown) while a dialog is open, so it is not closed from under its owner: the person
// answers it first. Refused too where pop_one would refuse (the update screen), so the count stays true. A screensaver
// or lock cover needs no care: back_step only touches the stack and show_top keeps the cover in front.
static void reuse_open(nav_open_t how, int frame, int screen_id) {
    if (how == NAV_OPEN_KEEP) {
        reused_count++;
    } else if (shell_dialog_group() || update_screen_blocks_back()) {
        ESP_LOGW(TAG, "screen %d is open below, not going back (a dialog or the update screen is up)", screen_id);
    } else if (!back_later(frames[frame].screen, NULL, NULL)) {
        ESP_LOGW(TAG, "screen %d is open below, going back to it failed", screen_id);
    } else {
        reused_count++;
    }
}

typedef struct {
    lv_obj_t *(*build)(void);
    lv_obj_t *screen;
    bool (*on_esc)(void *);
    void *on_esc_arg;
    void (*on_search)(void *);
    void *on_search_arg;
} build_job_t;

// lichen_ui_with_group's builder: the screen, its widgets in the new group, and the Esc and search hooks it set
static void build_in_group(void *arg) {
    build_job_t *job = arg;
    shell_on_esc(NULL, NULL);
    shell_on_search(NULL, NULL);
    job->screen        = job->build();
    job->on_esc        = building_esc;
    job->on_esc_arg    = building_esc_arg;
    job->on_search     = building_search;
    job->on_search_arg = building_search_arg;
    shell_on_esc(NULL, NULL);
    shell_on_search(NULL, NULL);
}

void shell_open_keyed_impl(lv_obj_t *(*build)(void), int screen_id, uint32_t key) {
    lichen_sys_ui_lock(0);
    if (!frames[0].screen || home_settling()) {
        ESP_LOGW(TAG, "screen %d not opened (still booting)", screen_id);
        lichen_sys_ui_unlock();
        return;
    }
    int        frame = -1;
    nav_open_t how   = nav_decide(screen_id, key, &frame);
    if (how != NAV_OPEN_NEW) {
        reuse_open(how, frame, screen_id);
        lichen_sys_ui_unlock();
        return;
    }
    if (!nav_push_keyed(screen_id, key)) {
        ESP_LOGW(TAG, "screen %d not opened (stack full)", screen_id);
        lichen_sys_ui_unlock();
        return;
    }
    lv_group_t *group = lv_group_create();
    build_job_t job   = {.build = build};
    lichen_ui_with_group(group, build_in_group, &job);
    lv_obj_t *screen = job.screen;
    if (!screen) {
        nav_back();
        lv_group_delete(group);
        lichen_sys_ui_unlock();
        return;
    }
    if (lv_group_get_obj_count(group) == 0) {
        lv_group_add_obj(group, screen); // keys still reach a screen without focusable widgets
        shell_no_arrow_scroll(screen);
    }
    frames[nav_depth() - 1] = (frame_t){screen, group, job.on_esc, job.on_esc_arg, job.on_search, job.on_search_arg};
    show_top();
    lichen_sys_ui_unlock();
}

void shell_close_all_but_top(void) {
    lichen_sys_ui_lock(0);
    int depth = nav_depth();
    if (depth <= 1 || !frames[0].screen) {
        lichen_sys_ui_unlock();
        return;
    }
    // The top screen is showing: delete the others only once it has its own slot, so none is deleted twice
    frame_t old[NAV_MAX_DEPTH];
    int     top_id = nav_current();
    uint32_t top_key = nav_key_at(depth - 1);
    frame_t top    = frames[depth - 1];
    memcpy(old, frames, sizeof(old));
    memset(frames, 0, sizeof(frames));
    nav_reset();
    nav_push_keyed(top_id, top_key);
    frames[1] = top;
    show_top();
    for (int i = 0; i < depth - 1; i++) {
        drop(&old[i]);
    }
    lichen_sys_ui_unlock();
}

void shell_attach_keys(lv_indev_t *indev) {
    if (!indev) {
        return;
    }
    lichen_sys_ui_lock(0);
    keys = indev;
    lv_indev_add_event_cb(keys, on_key, LV_EVENT_KEY, NULL);
    ui_perf_watch_keys(keys);
    screen_idle_start(); // no keyboard, no timeout: nothing could wake the screen
    if (frames[0].screen) {
        focus_keys();
    } else if (boot_screen_group()) {
        lv_indev_set_group(keys, boot_screen_group()); // any key shows the boot details
    }
    lichen_sys_ui_unlock();
}

// "fw keys log on": raw key events (before the keyboard maps it, so shape keys and Fn show too), only those captured while
// the Keyboard Test was open: anywhere else the codes could spell a password. Not persisted.
static bool log_raw;

static void on_raw_key(key_raw_event_t const *ev, void *arg) {
    (void)arg;
    if (!ev->test_open) {
        return;
    }
    char             text[64];
    lichen_raw_key_t key = {.code = ev->code, .pressed = ev->pressed, .shift = ev->shift};
    key_matrix_format(text, sizeof(text), &key);
    ESP_LOGI(TAG, "raw key %s", text);
}

static void set_raw_log(bool on) {
    lichen_sys_ui_lock(0);
    if (on && !log_raw) {
        log_raw = raw_keys_listen(on_raw_key, NULL, false);
    } else if (!on && log_raw) {
        raw_keys_unlisten(on_raw_key, NULL);
        log_raw = false;
    }
    lichen_sys_ui_unlock();
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

static bool boot_screen_shown;

void shell_boot_screen(lv_display_t *display) {
    shell_ui_ops_register(); // the boot screen styles itself through the gate (#221)
    lichen_sys_ui_lock(0);
    lv_display_set_default(display);
    ui_perf_early(display);
    lv_obj_t *boot = boot_screen_create();
    lv_screen_load_anim(boot, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true); // drops LVGL's empty start screen
    boot_screen_shown = true;
    lichen_sys_ui_unlock();
}

#define RESTART_FRAME_MS 2000

static char restart_note[160];

void shell_restart_note(char const *text) {
    snprintf(restart_note, sizeof(restart_note), "%s", text ? text : "");
}

char const *shell_restart_note_get(void) {
    return restart_note;
}

// The frame's screen, loaded but not drawn. LVGL lock held. False when it could not be built
static bool restart_frame_load(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_t *label  = screen ? lv_label_create(screen) : NULL;
    lv_obj_t *note   = label && restart_note[0] ? lv_label_create(screen) : NULL;
    if (!label) {
        if (screen) {
            lv_obj_delete(screen);
        }
        return false;
    }
    shell_style_screen(screen);
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_text_font(label, lichen_font(LICHEN_FONT_LARGE), 0);
    lv_label_set_text(label, "Restarting...");
    if (note) { // e.g. the factory reset's dark minute: said on the frame the user sees last
        lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(screen, 24, 0);
        lv_obj_set_width(note, LV_PCT(100));
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(note, lv_color_hex(THEME_AMBER), 0);
        lv_label_set_text(note, restart_note);
    } else {
        lv_obj_center(label);
    }
    lv_screen_load(screen); // the old screens stay: the restart frees everything
    return true;
}

void shell_show_restarting(void) {
    if (!lv_display_get_default() || !lichen_sys_ui_lock(300)) {
        return;
    }
    // A small stack (the 4 KiB console task) only loads the screen: drawing it there overflowed the stack (#255)
    restart_frame_t where = restart_frame_plan(lichen_sys_mem_largest(LICHEN_SYS_MEM_PSRAM), shell_stack_free_here(), shell_on_ui_task());
    if (where == RESTART_FRAME_SKIP || !restart_frame_load()) {
        lichen_sys_ui_unlock();
        return;
    }
    if (where == RESTART_FRAME_HERE) {
        lv_refr_now(NULL);
    }
    lichen_sys_ui_unlock(); // RESTART_FRAME_UI_TASK: the LVGL task draws it on its next pass, well inside the wait below
    // Long enough to judge by eye (#134): at 40 ms nobody saw it, only the dark panel after it. The shutdown handler turns the
    // backlight off next, before the feed stops
    vTaskDelay(pdMS_TO_TICKS(RESTART_FRAME_MS));
}

void shell_start(lv_display_t *display) {
    shell_ui_ops_register(); // the gate's navigation calls reach this shell from now on (#221); again if the boot screen did
    // The boot screen is already up (shell_boot_screen), so the LVGL task renders while this runs: every LVGL call
    // below (display events, timers, objects) needs the lock, or a realloc races the LVGL task's walk (#118)
    lichen_sys_ui_lock(0);
    if (!shell_note_ui_task()) {
        ESP_LOGW(TAG, "LVGL task not noted: a restart from it with little stack waits for a frame nobody draws");
    }
    home_init();
    setup_init();
    screen_idle_init();
    attention_init();
    screen_saver_init();
    console_register("keys", "keys on|off (log every key) | keys log on|off (log raw key events) | keys send <up down left right enter esc tab bksp wait square triangle cross circle cloud diamond text:..| focus:..| row:..| notfocus:a;b| needlock> (guards: the sequence stops when the focused row does not match, or the remote key lock is off, #190)", cmd_keys);
    ui_probe_init();
    ui_remote_start();
    ui_check_start();
    ui_perf_start();
    if (!getenv("TZ")) {
        settings_apply_tz(); // main.c normally did this already
    }
    settings_app_register();
    about_app_register();
    files_app_register();
    bench_app_register();
    stress_init(); // "fw stress" (#278): console only, no tile
    crashlog_app_register();
    power_app_register();
    notify_app_register();
    notes_app_register();
    lv_display_set_default(display);
    if (!boot_screen_shown) {
        lv_obj_t *boot = boot_screen_create();
        lv_screen_load_anim(boot, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
        boot_screen_shown = true;
    }
    update_screen_init();
    notify_ui_start();
    shortcuts_start();
    up = true;
    lichen_sys_ui_unlock();
}
