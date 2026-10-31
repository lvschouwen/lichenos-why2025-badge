// Keyboard Test (#81): every key of the badge keyboard as the TCA8418 reports it, including the ones the OS drops before
// LVGL (shape keys, Fn, Ctrl ...). The 8 x 10 matrix is drawn in the chip's order (key_matrix.h); a key lights while held
// and stays marked once seen, and the last event is spelled out. While the screen is open the keys do nothing else: Esc held for
// 2 s leaves.
#include "app_icons.h"
#include "esp_log.h"
#include "fonts.h"
#include "hold_exit.h"
#include "key_matrix.h"
#include "lichen_app.h"
#include "raw_keys.h"
#include "shell.h"
#include "shell_parts.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_W   60
#define KEY_H   44
#define KEY_GAP 4

#define CODES (KEY_MATRIX_ROWS * KEY_MATRIX_COLS)

typedef struct {
    lv_obj_t   *cell[CODES];
    bool        seen[CODES];
    bool        down[CODES];
    lv_obj_t   *event_line;
    lv_obj_t   *count_line;
    lv_obj_t   *hold_bar;
    lv_timer_t *timer;
    hold_exit_t hold;
    unsigned    events;
    bool        listening;
} keytest_t;

static void style_cell(keytest_t const *t, int index) {
    uint32_t bg = t->down[index] ? THEME_AMBER : t->seen[index] ? THEME_TEAL : THEME_TILE;
    lv_obj_set_style_bg_color(t->cell[index], lv_color_hex(bg), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(t->cell[index], 0),
                                lv_color_hex(t->down[index] || t->seen[index] ? THEME_BG : THEME_TEXT), 0);
}

static void on_raw(key_raw_event_t const *ev, void *arg) {
    keytest_t *t = arg;
    int        row, col;
    if (key_matrix_pos(ev->code, &row, &col)) {
        int index        = row * KEY_MATRIX_COLS + col;
        t->down[index]   = ev->pressed;
        t->seen[index]  |= ev->pressed;
        style_cell(t, index);
    }
    char text[80];
    key_matrix_format(text, sizeof(text), ev);
    lv_label_set_text(t->event_line, text);
    if (ev->pressed) {
        t->events++;
    }
    char count[48];
    snprintf(count, sizeof(count), "%u key presses", t->events);
    lv_label_set_text(t->count_line, count);
    hold_exit_event(&t->hold, ev->code == KEY_MATRIX_ESC_CODE, ev->pressed, lv_tick_get());
}

static void tick(lv_timer_t *timer) {
    keytest_t *t   = lv_timer_get_user_data(timer);
    uint32_t   now = lv_tick_get();
    lv_bar_set_value(t->hold_bar, hold_exit_percent(&t->hold, now), LV_ANIM_OFF);
    if (hold_exit_tick(&t->hold, now)) {
        shell_back();
    }
}

static void on_delete(lv_event_t *e) {
    keytest_t *t = lv_event_get_user_data(e);
    raw_keys_set_test_open(false);
    if (t->listening) {
        raw_keys_unlisten(on_raw, t);
    }
    lv_timer_delete(t->timer);
    free(t);
}

static lv_obj_t *add_label(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static void add_cell(keytest_t *t, lv_obj_t *grid, int row, int col) {
    int       index = row * KEY_MATRIX_COLS + col;
    lv_obj_t *cell  = lv_obj_create(grid);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, KEY_W, KEY_H);
    lv_obj_set_pos(cell, col * (KEY_W + KEY_GAP), row * (KEY_H + KEY_GAP));
    lv_obj_set_style_radius(cell, 6, 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *label = lv_label_create(cell);
    char      buf[8];
    lv_label_set_text(label, key_matrix_label(key_matrix_code(row, col), buf, sizeof(buf)));
    lv_obj_set_style_text_font(label, lichen_font(LICHEN_FONT_SMALL), 0);
    lv_obj_center(label);
    t->cell[index] = cell;
    style_cell(t, index);
}

static void open_keytest(lv_obj_t *screen) {
    keytest_t *t = calloc(1, sizeof(*t));
    if (!t) {
        add_label(screen, "Out of memory", THEME_RED);
        return;
    }
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 10, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = add_label(screen, "Keyboard test", THEME_TEXT);
    lv_obj_set_style_text_font(title, lichen_font(LICHEN_FONT_LARGE), 0);
    t->event_line = add_label(screen, "press any key", THEME_AMBER);
    t->count_line = add_label(screen, "0 key presses", THEME_DIM);

    lv_obj_t *grid = lv_obj_create(screen);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, KEY_MATRIX_COLS * (KEY_W + KEY_GAP) - KEY_GAP, KEY_MATRIX_ROWS * (KEY_H + KEY_GAP) - KEY_GAP);
    for (int row = 0; row < KEY_MATRIX_ROWS; row++) {
        for (int col = 0; col < KEY_MATRIX_COLS; col++) {
            add_cell(t, grid, row, col);
        }
    }

    add_label(screen, "Hold Esc for 2 s to leave", THEME_DIM);
    t->hold_bar = lv_bar_create(screen);
    lv_obj_set_size(t->hold_bar, LV_PCT(100), 10);
    lv_bar_set_range(t->hold_bar, 0, 100);
    lv_obj_set_style_bg_color(t->hold_bar, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_color(t->hold_bar, lv_color_hex(THEME_AMBER), LV_PART_INDICATOR);

    raw_keys_set_test_open(true);
    t->timer     = lv_timer_create(tick, 50, t);
    t->listening = raw_keys_listen(on_raw, t, true);
    if (!t->listening) {
        lv_label_set_text(t->event_line, "raw keys unavailable");
    }
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, t);
}

static bool keytest_tile(char *l1, size_t c1, char *l2, size_t c2) {
    snprintf(l1, c1, "every key, raw");
    snprintf(l2, c2, "hold Esc to leave");
    return true;
}

static lichen_app_t const keytest_app = {
    .name      = "Keyboard Test",
    .id        = "keytest",
    .draw_icon = icon_keytest,
    .tile_text = keytest_tile,
    .open      = open_keytest,
};

void keytest_app_register(void) {
    lichen_register_app(&keytest_app);
}
