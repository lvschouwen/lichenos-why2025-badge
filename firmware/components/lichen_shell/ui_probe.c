// Bench diagnostics (console only, #67): `fw keys send <seq>` feeds keys to the screen on top and `fw ui` prints what
// is on it, so a screen can be driven and read without anyone at the badge. Tokens: see ui_keys.h.
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nav.h"
#include "raw_keys.h"
#include "shell_parts.h"
#include "ui_keys.h"
#include "ui_overflow.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static_assert(LV_KEY_UP == 17 && LV_KEY_DOWN == 18 && LV_KEY_RIGHT == 19 && LV_KEY_LEFT == 20 && LV_KEY_ENTER == 10 &&
                  LV_KEY_ESC == 27 && LV_KEY_NEXT == 9 && LV_KEY_BACKSPACE == 8,
              "ui_keys.c's key codes must match LVGL's");

#define KEY_GAP_MS  250 // lets the shell's async screen changes finish between keys
#define WAIT_MS     1500
#define MAX_KEYS    128
#define MAX_LINES   64
#define LINE_CHARS  96

typedef struct {
    int32_t y, x;
    char    text[LINE_CHARS];
} line_t;

// LVGL context only
static void deliver_key(uint32_t key) {
    if (screen_idle_key()) {
        return; // it woke the dark screen (#43), as a real key would
    }
    if (screen_lock_active()) {
        screen_lock_probe_key(key); // the lock takes every key but feeds the unlock combo (#75, #112)
        return;
    }
    lv_group_t *group = shell_top_group();
    if (UI_KEY_IS_SHAPE(key)) {
        shortcuts_key(UI_KEY_SHAPE_CODE(key), true, raw_keys_test_open()); // as the real key: the OS's, never LVGL's
    } else if (key == LV_KEY_ESC) {
        shell_esc(); // the shell's own Esc handling (it sees the indev's key, which this does not go through)
    } else if (key == LV_KEY_NEXT) {
        if (group) {
            lv_group_focus_next(group);
        }
    } else if (group) {
        lv_group_send_data(group, key);
    }
}

// The key handlers (and what they build) run on the LVGL task, one key per timer tick: the console task's 4 KB stack
// is too small for them. The console task waits for the end.
typedef struct {
    uint32_t     keys[MAX_KEYS];
    size_t       n, next;
    TaskHandle_t waiter;
} sequence_t;

static void sequence_step(lv_timer_t *timer) {
    sequence_t *seq = lv_timer_get_user_data(timer);
    if (seq->next < seq->n) {
        uint32_t key = seq->keys[seq->next++];
        if (key == UI_KEY_WAIT) {
            lv_timer_set_period(timer, WAIT_MS);
            return;
        }
        deliver_key(key);
        lv_timer_set_period(timer, KEY_GAP_MS);
        return;
    }
    screen_lock_probe_end();
    xTaskNotifyGive(seq->waiter); // the last gap has passed: the screen has settled
    lv_timer_delete(timer);
}

// "fw keys send <seq>" (shell.c's keys command hands it over)
void ui_probe_send(char const *seq_text) {
    size_t      pos;
    sequence_t *seq = calloc(1, sizeof(*seq));
    if (!seq) {
        console_reply("keys send: no memory");
        return;
    }
    seq->n = ui_keys_parse(seq_text, seq->keys, MAX_KEYS, &pos);
    if (seq->n == UI_KEYS_ERR) {
        console_reply("keys send: not understood at offset %u", (unsigned)pos);
        free(seq);
        return;
    }
    seq->waiter = xTaskGetCurrentTaskHandle();
    ulTaskNotifyTake(pdTRUE, 0); // a stale notification would end the wait early
    lvgl_port_lock(0);
    lv_timer_t *timer = lv_timer_create(sequence_step, 1, seq);
    lvgl_port_unlock();
    if (!timer) {
        console_reply("keys send: no timer");
        free(seq);
        return;
    }
    uint32_t total_ms = (uint32_t)(seq->n + 2) * (KEY_GAP_MS + WAIT_MS);
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(total_ms)) == 0) {
        // Not finished (LVGL stuck?): the timer still owns seq, so leave it be rather than free it under the timer
        console_reply("keys send: timed out");
        return;
    }
    console_reply("keys sent: %u", (unsigned)seq->n);
    free(seq);
}

static bool visible(lv_obj_t *obj) {
    for (lv_obj_t *o = obj; o; o = lv_obj_get_parent(o)) {
        if (lv_obj_is_hidden(o)) {
            return false;
        }
    }
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return a.x2 >= 0 && a.y2 >= 0 && a.x1 < lv_display_get_horizontal_resolution(NULL) &&
           a.y1 < lv_display_get_vertical_resolution(NULL);
}

static void collect(lv_obj_t *obj, line_t *lines, size_t *n) {
    if (*n >= MAX_LINES) {
        return;
    }
    if (lv_obj_check_type(obj, &lv_label_class) && visible(obj)) {
        char const *text = lv_label_get_text(obj);
        if (text && *text) {
            lv_area_t a;
            lv_obj_get_coords(obj, &a);
            lines[*n].y = a.y1;
            lines[*n].x = a.x1;
            // "* ": a checked button's label (the active tab, a chosen option)
            lv_obj_t *parent  = lv_obj_get_parent(obj);
            bool      checked = parent && lv_obj_has_state(parent, LV_STATE_CHECKED);
            snprintf(lines[*n].text, LINE_CHARS, "%s%s", checked ? "* " : "", text);
            (*n)++;
        }
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        collect(lv_obj_get_child(obj, (int32_t)i), lines, n);
    }
}

static int by_position(void const *a, void const *b) {
    line_t const *l = a, *r = b;
    return l->y != r->y ? (l->y < r->y ? -1 : 1) : (l->x < r->x ? -1 : l->x > r->x);
}

static void first_label(lv_obj_t *obj, char *out, size_t cap) {
    if (out[0]) {
        return;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        snprintf(out, cap, "%s", lv_label_get_text(obj));
        return;
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count && !out[0]; i++) {
        first_label(lv_obj_get_child(obj, (int32_t)i), out, cap);
    }
}

// A list that keeps the group focus on itself marks its highlighted row with LV_STATE_USER_1 (mc_list_style_row, #145):
// the row is what Enter would open, so that is the focus to report
static lv_obj_t *highlighted_child(lv_obj_t *foc) {
    uint32_t count = lv_obj_get_child_count(foc);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(foc, (int32_t)i);
        if (lv_obj_has_state(child, LV_STATE_USER_1)) {
            return child;
        }
    }
    return foc;
}

// What the focus object stands for when its own first label is not it: the home screen holds the focus for all its
// tiles, a tabview for all its tabs (#27)
static void describe_focus(lv_obj_t *foc, char *out, size_t cap) {
    char const *home = (nav_depth() == 1 && foc == lv_screen_active() && shell_home_shown()) ? home_selected_name(foc)
                                                                                             : NULL;
    if (home) {
        snprintf(out, cap, "%s", home);
    } else if (lv_obj_check_type(foc, &lv_tabview_class)) {
        lv_obj_t *button = lv_obj_get_child(lv_tabview_get_tab_bar(foc), (int32_t)lv_tabview_get_tab_active(foc));
        if (button) {
            first_label(button, out, cap);
        }
    } else {
        first_label(highlighted_child(foc), out, cap);
    }
}

static void settle_done(void *arg) {
    xTaskNotifyGive((TaskHandle_t)arg);
}

// Waits until the LVGL task has run what is queued for it (lv_async_call: Enter's screen change, and what that chains
// into), so the report below is of the keys already sent, not one behind
static void settle(void) {
    for (int round = 0; round < 3; round++) {
        ulTaskNotifyTake(pdTRUE, 0);
        lvgl_port_lock(0);
        lv_async_call(settle_done, xTaskGetCurrentTaskHandle());
        lvgl_port_unlock();
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(300)) == 0) {
            return;
        }
    }
}

// ---- "fw ui overflow" (#133): labels whose text is cut off -------------------------------------------------------------

#define OVF_TEXT_CHARS 60

typedef struct {
    int32_t px, have, x, y;
    char    text[OVF_TEXT_CHARS + 4];
} hit_t;

static ovf_mode_t overflow_mode(lv_label_long_mode_t m) {
    switch (m) {
    case LV_LABEL_LONG_MODE_CLIP:
        return OVF_CLIP;
    case LV_LABEL_LONG_MODE_DOTS:
        return OVF_DOT;
    case LV_LABEL_LONG_MODE_SCROLL:
    case LV_LABEL_LONG_MODE_SCROLL_CIRCULAR:
        return OVF_SCROLL;
    default:
        return OVF_WRAP;
    }
}

// The label's visible box: its coords cut by every ancestor's coords and the screen
static void visible_box(lv_obj_t *obj, lv_area_t *box) {
    lv_obj_get_coords(obj, box);
    for (lv_obj_t *p = lv_obj_get_parent(obj); p; p = lv_obj_get_parent(p)) {
        lv_area_t a;
        lv_obj_get_coords(p, &a);
        box->x1 = LV_MAX(box->x1, a.x1);
        box->y1 = LV_MAX(box->y1, a.y1);
        box->x2 = LV_MIN(box->x2, a.x2);
        box->y2 = LV_MIN(box->y2, a.y2);
    }
    box->x1 = LV_MAX(box->x1, 0);
    box->y1 = LV_MAX(box->y1, 0);
    box->x2 = LV_MIN(box->x2, lv_display_get_horizontal_resolution(NULL) - 1);
    box->y2 = LV_MIN(box->y2, lv_display_get_vertical_resolution(NULL) - 1);
}

static void overflow_walk(lv_obj_t *obj, hit_t *hits, size_t *n, size_t *total) {
    if (lv_obj_is_hidden(obj)) {
        return;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        char const *text = lv_label_get_text(obj);
        lv_area_t   a, box;
        lv_obj_get_coords(obj, &a);
        visible_box(obj, &box);
        if (text && *text && box.x1 <= box.x2 && box.y1 <= box.y2 && a.y2 >= box.y1 && a.y1 <= box.y2) {
            lv_point_t size;
            lv_text_get_size(&size, text, lv_obj_get_style_text_font(obj, LV_PART_MAIN),
                             lv_obj_get_style_text_letter_space(obj, LV_PART_MAIN),
                             lv_obj_get_style_text_line_space(obj, LV_PART_MAIN), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            int32_t have = 0;
            int32_t over = ui_overflow_px(size.x, (ovf_span_t){a.x1, a.x2}, (ovf_span_t){box.x1, box.x2},
                                          overflow_mode(lv_label_get_long_mode(obj)), &have);
            if (over > 0) {
                (*total)++;
                if (*n < MAX_LINES) {
                    hit_t *h = &hits[(*n)++];
                    h->px    = size.x;
                    h->have  = have;
                    h->x     = a.x1;
                    h->y     = a.y1;
                    snprintf(h->text, sizeof(h->text), "%.*s", OVF_TEXT_CHARS, text);
                    for (char *c = h->text; *c; c++) {
                        if (*c == '\n' || *c == '\r') {
                            *c = ' ';
                        }
                    }
                }
            }
        }
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        overflow_walk(lv_obj_get_child(obj, (int32_t)i), hits, n, total);
    }
}

// The active screen, then the top layer (status bar, toast, dialogs) and the sys layer
static void cmd_ui_overflow(void) {
    settle();
    hit_t *hits = malloc(sizeof(hit_t) * MAX_LINES);
    if (!hits) {
        console_reply("ui overflow: no memory");
        return;
    }
    size_t n = 0, total = 0;
    lvgl_port_lock(0);
    overflow_walk(lv_screen_active(), hits, &n, &total);
    overflow_walk(lv_layer_top(), hits, &n, &total);
    overflow_walk(lv_layer_sys(), hits, &n, &total);
    lvgl_port_unlock();
    console_reply("ui overflow: %u", (unsigned)total);
    for (size_t i = 0; i < n; i++) {
        console_reply("  %d,%d %d/%d px \"%s\"", (int)hits[i].x, (int)hits[i].y, (int)hits[i].px, (int)hits[i].have,
                      hits[i].text);
    }
    free(hits);
}

static void cmd_ui(char const *args) {
    if (strncmp(args, "perf", 4) == 0) {
        ui_perf_cmd(args + 4);
        return;
    }
    if (strncmp(args, "overflow", 8) == 0) {
        cmd_ui_overflow();
        return;
    }
    settle();
    line_t *lines = malloc(sizeof(line_t) * MAX_LINES);
    if (!lines) {
        console_reply("ui: no memory");
        return;
    }
    size_t n = 0;
    char   stack[64] = "", focus[LINE_CHARS] = "";
    lvgl_port_lock(0);
    for (int i = 0; i < nav_depth(); i++) {
        size_t used = strlen(stack);
        snprintf(stack + used, sizeof(stack) - used, "%s%d", i ? " > " : "", nav_at(i));
    }
    lv_group_t *group = shell_top_group();
    lv_obj_t   *foc   = group ? lv_group_get_focused(group) : NULL;
    if (foc) {
        describe_focus(foc, focus, sizeof(focus));
    }
    collect(lv_screen_active(), lines, &n);
    lvgl_port_unlock();
    qsort(lines, n, sizeof(line_t), by_position);
    console_reply("ui stack: %s | focus: %s", stack, focus[0] ? focus : "(none)");
    for (size_t i = 0; i < n; i++) {
        console_reply("ui   %s", lines[i].text);
    }
    free(lines);
}

void ui_probe_init(void) {
    console_register("ui", "ui [perf [reset | bench [n]] | overflow] (overflow: labels whose text is cut off, #133; screen ids on the stack, the focused row, every visible label; perf: frame timing, #93)", cmd_ui);
}
