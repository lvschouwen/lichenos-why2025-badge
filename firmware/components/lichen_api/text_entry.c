// Full-screen text entry (#66): see text_entry.h
#include "secure_wipe.h"
#include "text_entry.h"
#include "esp_log.h"
#include "key_cause.h"
#include "screen_ids.h"

#include "lichen_ui.h"
#include "fonts.h"
#include "theme.h"
#include "ui_text.h"

#include <stdio.h>
#include <string.h>

#define MAX_CHARS         128

static char   title[160];
static char   initial[MAX_CHARS + 1];
static bool   secret;
static size_t max_len;
static void (*done_cb)(char const *, void *);
static void *done_arg;

static lv_obj_t   *ta;
static lv_indev_t *tab_indev;
static bool        fired;
static bool        typed_remotely; // a change since the open that was no hand's (#405); cleared only by a new open
static char        pending[MAX_CHARS + 1];

// Runs from shell_back_then, once the entry screen is gone: then the owner hears (and may open a screen of its own)
static void finish(void *unused) {
    (void)unused;
    if (done_cb) {
        done_cb(pending, done_arg);
    }
    memset(pending, 0, sizeof(pending));
}

// The text's changes (a character, Backspace, Del) run while LVGL handles the key that made them, so its cause is that
// key's. SYSTEM counts as remote too: no console command types into a field, and any that did is no hand.
static void on_changed(lv_event_t *e) {
    (void)e;
    if (key_cause_now() != KEY_CAUSE_HAND) {
        typed_remotely = true;
    }
}

bool text_entry_typed_by_hand(void) {
    return !typed_remotely;
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
    if (ta) {
        char *text = (char *)lv_textarea_get_text(ta); // LVGL's own buffer: it is freed with the widget, not cleared
        secure_wipe(text, strlen(text));
    }
    secure_wipe(initial, sizeof(initial));
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

    // A heading may carry a name on a second line ("Password for\n<name>"): the first line is the kind (small, dim),
    // the name is the heading proper, LARGE on its own wrapping lines, so a long name is never lost behind a "..."
    // (#132, R1/#135). Without a name the whole text is the one LARGE line.
    char const *nl = strchr(title, '\n');
    if (nl && nl[1]) {
        char kind[sizeof(title)];
        snprintf(kind, sizeof(kind), "%.*s", (int)(nl - title), title);
        lichen_heading(screen, kind, nl + 1);
    } else {
        char one[sizeof(title)];
        snprintf(one, sizeof(one), "%.*s", nl ? (int)(nl - title) : (int)sizeof(one) - 1, title);
        lichen_heading(screen, NULL, one);
    }

    ta = lv_textarea_create(screen);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, (uint32_t)max_len);
    lv_textarea_set_password_mode(ta, secret);
    if (secret) {
        lv_obj_set_user_flag(ta, TEXT_ENTRY_SECRET_BIT, true);
    }
    lv_textarea_set_text(ta, initial);
    lv_obj_set_style_bg_color(ta, lv_color_hex(THEME_TILE), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_font(ta, lichen_font(LICHEN_FONT_SMALL), 0);
    lv_obj_set_style_radius(ta, 14, 0);
    lv_obj_set_style_pad_all(ta, 18, 0);
    lv_obj_set_style_border_width(ta, 3, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_outline_width(ta, 0, LV_STATE_ANY);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_bg_color(ta, lv_color_hex(THEME_AMBER), LV_PART_CURSOR);
    lv_obj_set_style_text_color(ta, lv_color_hex(THEME_BG), LV_PART_CURSOR);
    lv_obj_add_event_cb(ta, on_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ta, on_changed, LV_EVENT_VALUE_CHANGED, NULL); // after the initial text: that is no typing
    lv_obj_add_event_cb(ta, on_delete, LV_EVENT_DELETE, NULL);

    lv_obj_t *hint = lv_label_create(screen);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_color(hint, lv_color_hex(THEME_DIM), 0);
    char hint_text[64];
    if (secret) {
        ui_hint_build(hint_text, sizeof(hint_text), "Enter", "done", "Esc", "cancel", "Tab", "show / hide", NULL);
    } else {
        ui_hint_build(hint_text, sizeof(hint_text), "Enter", "done", "Esc", "cancel", NULL);
    }
    lv_label_set_text(hint, hint_text);

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

bool text_entry_open(char const *heading, char const *start, bool is_secret, size_t max, void (*done)(char const *, void *),
                     void *arg) {
    if (shell_screen_open(SCREEN_TEXT_ENTRY, 0)) { // its statics belong to the open one: never hand them to a new owner
        ESP_LOGW("text_entry", "an entry is already open, not opening \"%s\"", heading ? heading : "");
        return false;
    }
    snprintf(title, sizeof(title), "%s", heading ? heading : "");
    snprintf(initial, sizeof(initial), "%s", start ? start : "");
    secret         = is_secret;
    typed_remotely = false;
    max_len        = max > MAX_CHARS ? MAX_CHARS : max;
    done_cb        = done;
    done_arg       = arg;
    shell_open(build, SCREEN_TEXT_ENTRY);
    return true;
}
