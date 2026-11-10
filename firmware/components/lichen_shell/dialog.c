// The shell's dialog layer (#14 T-B): one OS warning at a time on lv_layer_top(), over every screen and the status
// bar, with its own keypad group. Enter confirms, Esc cancels, and a countdown confirms by itself when nobody answers
// (dialog_countdown.c, host-tested). Other tasks never touch LVGL here: they set the wanted state under a spinlock and
// queue one async call (lichen_sys_ui_call, or lv_async_call on the LVGL task) that makes the screen match it, so an
// open and a close queued together still end right whatever order the calls run in (UI-service calls are FIFO, raw
// lv_async_call newest first, and the two do not order against each other: see shell.c).
#include "dialog_countdown.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "lichen_sys_ui.h"
#include "esp_timer.h"
#include "fonts.h"
#include "freertos/FreeRTOS.h"
#include "shell_parts.h"
#include "theme.h"
#include "ui_text.h"
#include "shell_ui_ops.h"

#include <stdio.h>
#include <string.h>

static char const TAG[] = "dialog";

#define TEXT_MAX     192 // a command echoed back in the CLI confirm is up to ~145 characters (#132)
#define TICK_MS      250
#define LOCK_WAIT_MS 2000

typedef struct {
    char     title[TEXT_MAX]; // shown as "<title> <lead>(cancels in N s)", see ui_countdown_text
    char     prefix[TEXT_MAX];
    char     suffix[TEXT_MAX]; // unused since #135 (kept for the struct)
    char     hint[TEXT_MAX];
    char     confirm_label[TEXT_MAX / 4];
    char     cancel_label[TEXT_MAX / 4];
    uint32_t seconds;
    bool     careful;
    shell_dialog_fn done;
} request_t;

// Wanted state, written by any task under want_lock; read by apply() on the LVGL task
static portMUX_TYPE want_lock = portMUX_INITIALIZER_UNLOCKED;
static bool         want_open;
static uint32_t     want_gen; // bumped by every open, so a second open replaces the first
static request_t    want;

// LVGL task only
static lv_obj_t        *backdrop;
static lv_obj_t        *message;
static lv_group_t      *group;
static lv_timer_t      *timer;
static dlg_countdown_t  countdown;
EXT_RAM_BSS_ATTR static request_t        shown;
static uint32_t         shown_gen;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void set_message(void) {
    char text[2 * TEXT_MAX + 40];
    ui_countdown_text(text, sizeof(text), shown.title, shown.prefix, shown.careful, shown.confirm_label,
                      dlg_countdown_left_s(&countdown, now_ms()));
    if (strcmp(lv_label_get_text(message), text) != 0) {
        lv_label_set_text(message, text);
    }
}

static void teardown(void) {
    if (!backdrop) {
        return;
    }
    lv_timer_delete(timer);
    lv_group_delete(group);
    lv_obj_delete(backdrop);
    backdrop = NULL;
    message  = NULL;
    group    = NULL;
    timer    = NULL;
    shell_refocus();
}

static void apply(void *arg);

// The answer: the screen closes first (later, outside the key handler that may be running), then the owner hears it
static void decided(bool confirmed) {
    shell_dialog_fn done = shown.done;
    uint32_t        gen  = shown_gen;
    portENTER_CRITICAL(&want_lock);
    if (want_gen == gen) {
        want_open = false;
    }
    portEXIT_CRITICAL(&want_lock);
    lv_async_call(apply, NULL);
    ESP_LOGI(TAG, "answered: %s", confirmed ? "confirm" : "cancel");
    if (done) {
        done(confirmed);
    }
}

static void on_tick(lv_timer_t *t) {
    (void)t;
    dlg_result_t r = dlg_countdown_tick(&countdown, now_ms());
    if (r != DLG_WAIT) {
        decided(r == DLG_CONFIRM);
        return;
    }
    set_message();
}

static void on_key(lv_event_t *e) {
    uint32_t     key = lv_event_get_key(e);
    dlg_key_t    k   = key == LV_KEY_ENTER ? DLG_KEY_ENTER : key == LV_KEY_ESC ? DLG_KEY_ESC : DLG_KEY_OTHER;
    dlg_result_t r   = dlg_countdown_key(&countdown, k, now_ms());
    if (r != DLG_WAIT) {
        decided(r == DLG_CONFIRM);
    }
}

// LVGL sends RELEASED to the focused object when Enter goes up (the keyboard driver reports every key's release)
static void on_released(lv_event_t *e) {
    (void)e;
    dlg_countdown_enter_up(&countdown);
}

static lv_obj_t *add_label(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *build_panel(lv_obj_t *parent) {
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_PCT(84), LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 3, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_pad_all(panel, 32, 0);
    lv_obj_set_style_pad_row(panel, 24, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(panel, false);
    lv_obj_set_clickable(panel, false);
    return panel;
}

static void build(void) {
    backdrop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(backdrop);
    lv_obj_set_size(backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(backdrop, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_80, 0);
    lv_obj_set_style_text_font(backdrop, lichen_font(LICHEN_FONT_SMALL), 0);
    lv_obj_set_scrollable(backdrop, false);
    // No focus ring on the backdrop: it only collects the keys
    lv_obj_set_style_outline_width(backdrop, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(backdrop, 0, LV_STATE_FOCUS_KEY);
    lv_obj_t *panel = build_panel(backdrop);
    message         = add_label(panel, "", THEME_TEXT);
    // R8: amber means attention, so only a careful dialog's hint has it; the rest is a dim key hint
    char hint[2 * TEXT_MAX];
    if (shown.hint[0]) {
        snprintf(hint, sizeof(hint), "%s", shown.hint);
    } else {
        ui_confirm_hint(hint, sizeof(hint), shown.confirm_label[0] ? shown.confirm_label : "continue",
                        shown.cancel_label[0] ? shown.cancel_label : NULL);
    }
    add_label(panel, hint, shown.careful ? THEME_AMBER : THEME_DIM);
    group = lv_group_create();
    lv_group_add_obj(group, backdrop);
    lv_obj_add_event_cb(backdrop, on_key, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(backdrop, on_released, LV_EVENT_RELEASED, NULL);
    timer = lv_timer_create(on_tick, TICK_MS, NULL);
    if (shown.careful) {
        dlg_countdown_start_careful(&countdown, now_ms(), shown.seconds);
    } else {
        dlg_countdown_start(&countdown, now_ms(), shown.seconds);
    }
    set_message();
    shell_refocus();
    // shell_refocus made the keypad wait for a release: LVGL drops every press until the keys are up, so the first
    // Enter this dialog gets is a fresh press, even when the release itself was swallowed by that wait
    dlg_countdown_enter_up(&countdown);
}

// Makes the screen match the wanted state
static void apply(void *arg) {
    (void)arg;
    portENTER_CRITICAL(&want_lock);
    bool      open = want_open;
    uint32_t  gen  = want_gen;
    request_t req  = want;
    portEXIT_CRITICAL(&want_lock);
    if (backdrop && (!open || gen != shown_gen)) {
        teardown();
    }
    if (open && !backdrop) {
        shown     = req;
        shown_gen = gen;
        build();
    }
}

static bool queue_apply(void) {
    return lichen_sys_ui_call(apply, NULL, LOCK_WAIT_MS);
}

// A bounded copy into one TEXT_MAX field, NULL as "": no snprintf with interrupts off (#5)
static void copy_text(char *dst, size_t max, char const *src) {
    size_t n = src ? strnlen(src, max - 1) : 0;
    memcpy(dst, src ? src : "", n);
    dst[n] = '\0';
}

bool shell_dialog_countdown_impl(shell_dialog_t const *d, shell_dialog_fn done) {
    portENTER_CRITICAL(&want_lock);
    copy_text(want.title, TEXT_MAX, d->title);
    copy_text(want.prefix, TEXT_MAX, d->count_prefix);
    copy_text(want.suffix, TEXT_MAX, d->count_suffix);
    copy_text(want.hint, TEXT_MAX, d->hint);
    copy_text(want.confirm_label, sizeof(want.confirm_label), d->confirm_label);
    copy_text(want.cancel_label, sizeof(want.cancel_label), d->cancel_label);
    want.seconds = d->seconds;
    want.careful = d->careful;
    want.done    = done;
    want_open    = true;
    want_gen++;
    portEXIT_CRITICAL(&want_lock);
    bool ok = queue_apply();
    if (!ok) {
        portENTER_CRITICAL(&want_lock);
        want_open = false;
        portEXIT_CRITICAL(&want_lock);
        ESP_LOGE(TAG, "could not be shown (LVGL busy)");
    }
    return ok;
}

void shell_dialog_close_impl(void) {
    portENTER_CRITICAL(&want_lock);
    bool was = want_open;
    want_open = false;
    portEXIT_CRITICAL(&want_lock);
    if (was) {
        queue_apply();
    }
}

lv_group_t *shell_dialog_group(void) {
    return group;
}
