// Full-screen text entry (#66): see text_entry.h
#include "text_entry.h"

#include "shell_parts.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define SCREEN_TEXT_ENTRY 120
#define MAX_CHARS         128

static char   title[64];
static char   initial[MAX_CHARS + 1];
static bool   secret;
static size_t max_len;
static void (*done_cb)(char const *, void *);
static void *done_arg;

static lv_obj_t   *ta;
static lv_indev_t *tab_indev;
static bool        fired;
static char        pending[MAX_CHARS + 1];

// Runs from shell_back_then, once the entry screen is gone: then the owner hears (and may open a screen of its own)
static void finish(void *unused) {
    (void)unused;
    if (done_cb) {
        done_cb(pending, done_arg);
    }
    memset(pending, 0, sizeof(pending));
}

static void on_ready(lv_event_t *e) {
    (void)e;
    if (fired || !ta) {
        return;
    }
    fired = true;
    snprintf(pending, sizeof(pending), "%s", lv_textarea_get_text(ta));
    shell_back_then(finish, NULL);
}

// Tab reaches the keypad indev's own event, not the textarea (LVGL turns it into a focus move)
static void on_indev_key(lv_event_t *e) {
    lv_indev_t *indev = lv_event_get_target(e);
    if (ta && secret && lv_indev_get_key(indev) == LV_KEY_NEXT && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
        lv_textarea_set_password_mode(ta, !lv_textarea_get_password_mode(ta));
    }
}

static void on_delete(lv_event_t *e) {
    (void)e;
    if (tab_indev) {
        lv_indev_remove_event_cb_with_user_data(tab_indev, on_indev_key, NULL);
        tab_indev = NULL;
    }
    ta = NULL;
}

static lv_obj_t *build(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    shell_style_screen(screen);
    lv_obj_set_style_pad_top(screen, THEME_STATUS_BAR_H + THEME_PAD, 0);
    lv_obj_set_style_pad_hor(screen, THEME_PAD, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 24, 0);

    lv_obj_t *heading = lv_label_create(screen);
    lv_obj_set_width(heading, LV_PCT(100));
    lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_40, 0);
    lv_label_set_text(heading, title);

    ta = lv_textarea_create(screen);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, (uint32_t)max_len);
    lv_textarea_set_password_mode(ta, secret);
    lv_textarea_set_text(ta, initial);
    lv_obj_set_style_bg_color(ta, lv_color_hex(THEME_TILE), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_24, 0);
    lv_obj_set_style_radius(ta, 14, 0);
    lv_obj_set_style_pad_all(ta, 18, 0);
    lv_obj_set_style_border_width(ta, 3, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_outline_width(ta, 0, LV_STATE_ANY);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_bg_color(ta, lv_color_hex(THEME_AMBER), LV_PART_CURSOR);
    lv_obj_set_style_text_color(ta, lv_color_hex(THEME_BG), LV_PART_CURSOR);
    lv_obj_add_event_cb(ta, on_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ta, on_delete, LV_EVENT_DELETE, NULL);

    lv_obj_t *hint = lv_label_create(screen);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_color(hint, lv_color_hex(THEME_DIM), 0);
    lv_label_set_text(hint, secret ? "Enter: done    Esc: cancel    Tab: show / hide" : "Enter: done    Esc: cancel");

    fired = false;
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_KEYPAD) {
            tab_indev = i;
            lv_indev_add_event_cb(i, on_indev_key, LV_EVENT_KEY, NULL);
            break;
        }
    }
    return screen;
}

void text_entry_open(char const *heading, char const *start, bool is_secret, size_t max, void (*done)(char const *, void *),
                     void *arg) {
    snprintf(title, sizeof(title), "%s", heading ? heading : "");
    snprintf(initial, sizeof(initial), "%s", start ? start : "");
    secret   = is_secret;
    max_len  = max > MAX_CHARS ? MAX_CHARS : max;
    done_cb  = done;
    done_arg = arg;
    shell_open(build, SCREEN_TEXT_ENTRY);
}
