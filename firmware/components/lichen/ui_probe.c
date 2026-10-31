// Bench diagnostics (console only, #67): `fw keys send <seq>` feeds keys to the screen on top and `fw ui` prints what
// is on it, so a screen can be driven and read without anyone at the badge. Tokens: see ui_keys.h.
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nav.h"
#include "shell_parts.h"
#include "ui_keys.h"

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
    lv_group_t *group = shell_top_group();
    if (key == LV_KEY_ESC) {
        shell_back(); // the shell's own Esc handling (it sees the indev's key, which this does not go through)
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

static void cmd_ui(char const *args) {
    (void)args;
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
        first_label(foc, focus, sizeof(focus));
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
    console_register("ui", "ui (screen ids on the stack, the focused row, every visible label)", cmd_ui);
}
