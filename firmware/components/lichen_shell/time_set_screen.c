// Settings > Time > Set date & time (#64): renders time_fields.h and sets the clock by hand through lichen_clock. No
// focusable widgets: shell_open puts the screen itself in the group, so every key arrives here as LV_EVENT_KEY.
// Up/Down change the field under the cursor (amber), Left/Right move, digits type, Backspace restarts the field,
// Enter sets the clock (seconds :00), Esc is the shell's (cancel).
#include "clock_local.h"
#include "fonts.h"
#include "lichen_clock.h"
#include "lichen_ui.h"
#include "settings_store.h"
#include "shell_parts.h"
#include "theme.h"
#include "time_fields.h"
#include "tz_table.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static time_fields_t tf;
static lv_obj_t     *field_label[TF_COUNT];
static lv_obj_t     *utc_label;
static lv_obj_t     *hint_label;

static lv_obj_t *add_label(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

// The fields as they would be set: a part-typed field counts as finished (a copy, the cursor stays)
static clock_local_t pending_utc(int64_t *utc) {
    time_fields_t t = tf;
    tf_commit(&t);
    return clock_local_to_utc(t.v[TF_YEAR], t.v[TF_MONTH], t.v[TF_DAY], t.v[TF_HOUR], t.v[TF_MINUTE], utc);
}

// "= 2026-10-31 13:05 UTC, Saturday": a typo in the day or the hour shows up here
static void show_utc(int64_t utc) {
    char      text[64] = "";
    time_t    t        = (time_t)utc;
    struct tm g;
    if (gmtime_r(&t, &g)) {
        strftime(text, sizeof(text), "= %Y-%m-%d %H:%M UTC, %A", &g);
    }
    lv_label_set_text(utc_label, text);
}

// The DST hint and the "earlier than before" hint, one per line
static void show_hints(clock_local_t how, int64_t utc) {
    char        text[160] = "";
    char const *local     = clock_local_hint(how);
    char const *back      = clock_is_set() ? clock_back_hint((int64_t)time(NULL), utc) : NULL;
    if (!clock_in_window(utc, clock_build_epoch())) {
        snprintf(text, sizeof(text), "outside the dates this firmware accepts");
    } else {
        snprintf(text, sizeof(text), "%s%s%s", local ? local : "", local && back ? "\n" : "", back ? back : "");
    }
    lv_label_set_text(hint_label, text);
}

static void render(void) {
    for (int f = 0; f < TF_COUNT; f++) {
        char text[8];
        tf_field_text(&tf, f, text, sizeof(text));
        lv_label_set_text(field_label[f], text);
        lv_obj_set_style_text_color(field_label[f], lv_color_hex(f == tf.cursor ? THEME_AMBER : THEME_TEXT), 0);
    }
    int64_t       utc = 0;
    clock_local_t how = pending_utc(&utc);
    show_utc(utc);
    show_hints(how, utc);
}

static void refused(clock_verdict_t v) {
    char text[96];
    if (v == CLOCK_OUTRANKED && clock_sync_info().source == CLOCK_SRC_SNTP) {
        snprintf(text, sizeof(text), "not set: SNTP set the clock %lld min ago", (long long)(clock_source_age_s() / 60));
    } else {
        snprintf(text, sizeof(text), "not set: %s", clock_verdict_text(v));
    }
    lv_label_set_text(hint_label, text);
}

static bool done; // an accepted set: the page is on its way out, further keys are ignored

static void set_clock(void) {
    if (done) {
        return;
    }
    tf_commit(&tf);
    int64_t utc = 0;
    pending_utc(&utc);
    struct timeval  tv = {.tv_sec = (time_t)utc};
    clock_verdict_t v  = clock_set_manual(&tv, false);
    if (clock_accepted(v)) {
        done = true;
        shell_back(); // later, outside the key handling; Time's page refreshes itself
        return;
    }
    render();
    refused(v);
}

static void on_key(lv_event_t *e) {
    if (done) {
        return;
    }
    uint32_t key = lv_event_get_key(e);
    switch (key) {
        case LV_KEY_UP: tf_step(&tf, 1); break;
        case LV_KEY_DOWN: tf_step(&tf, -1); break;
        case LV_KEY_LEFT: tf_move(&tf, -1); break;
        case LV_KEY_RIGHT: tf_move(&tf, 1); break;
        case LV_KEY_BACKSPACE: tf_backspace(&tf); break;
        case LV_KEY_ENTER: set_clock(); return;
        default:
            if (key < '0' || key > '9') {
                return;
            }
            tf_digit(&tf, (int)(key - '0'));
            break;
    }
    render();
}

// Local now when the clock is set (by anyone), else noon on the build's date: the field a person changes least is
// then already close. Years: the build's .. +2, the clock's plausible window.
static void prefill(void) {
    time_t    build = (time_t)clock_build_epoch();
    time_t    now   = time(NULL);
    struct tm b, l;
    if (!localtime_r(&build, &b)) {
        memset(&b, 0, sizeof(b));
        b.tm_year = 126, b.tm_mon = 0, b.tm_mday = 1;
    }
    int ymin = b.tm_year + 1900;
    if (clock_is_set() && localtime_r(&now, &l)) {
        tf_init(&tf, l.tm_year + 1900, l.tm_mon + 1, l.tm_mday, l.tm_hour, l.tm_min, ymin, ymin + 2);
    } else {
        tf_init(&tf, ymin, b.tm_mon + 1, b.tm_mday, 12, 0, ymin, ymin + 2);
    }
}

// Y - M - D   h : m, each field its own label so the one under the cursor can be amber
static void add_fields(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_text_font(row, lichen_font(LICHEN_FONT_LARGE), 0);
    lv_obj_set_style_pad_ver(row, 24, 0);
    static char const *const sep[TF_COUNT] = {"-", "-", "    ", ":", ""};
    for (int f = 0; f < TF_COUNT; f++) {
        field_label[f] = add_label(row, "", THEME_TEXT);
        if (sep[f][0] == ' ') { // a label of spaces has no width in a flex row: a fixed gap instead
            lv_obj_t *gap = lv_obj_create(row);
            lv_obj_remove_style_all(gap);
            lv_obj_set_size(gap, 28, 1);
        } else if (sep[f][0]) {
            add_label(row, sep[f], THEME_DIM);
        }
    }
}

static void zone_line(char *out, size_t cap) {
    char posix[64];
    settings_get_str("tz", posix, sizeof(posix), TZ_DEFAULT_POSIX);
    int i = tz_find_posix(posix);
    snprintf(out, cap, "Time zone: %s", i >= 0 ? tz_at((size_t)i)->name : posix);
}

lv_obj_t *time_set_build(void) {
    done = false;
    lv_obj_t *screen = shell_page_create();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 8, 0);
    lv_obj_set_scrollable(screen, false); // it fits; LVGL would otherwise take Up/Down as scrolling too
    lv_obj_t *title = add_label(screen, "Set date & time", THEME_TEXT);
    lv_obj_set_style_text_font(title, lichen_font(LICHEN_FONT_LARGE), 0);
    add_fields(screen);
    char zone[96];
    zone_line(zone, sizeof(zone));
    add_label(screen, zone, THEME_DIM);
    utc_label  = add_label(screen, "", THEME_DIM);
    hint_label = add_label(screen, "", THEME_AMBER);
    lv_obj_set_width(hint_label, LV_PCT(100));
    lv_label_set_long_mode(hint_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *help = add_label(screen, "Up/Down change, Left/Right move, digits type, Enter sets, Esc cancels",
                               THEME_DIM);
    lv_obj_set_width(help, LV_PCT(100));
    lv_label_set_long_mode(help, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_top(help, 24, 0);
    prefill();
    render();
    lv_obj_add_event_cb(screen, on_key, LV_EVENT_KEY, NULL);
    return screen;
}
