// Keyboard Test (#81): every key of the badge keyboard as the TCA8418 reports it, including the ones the OS drops before
// LVGL (shape keys, Fn, Ctrl ...). The keys are drawn like the printed keyboard (key_layout() in key_matrix.h, the shape
// keys in their printed colours); a key lights while held and stays marked once seen, and the last event is spelled out.
// While the screen is open the keys do nothing else: Esc held for 2 s leaves. The raw keys come on the LVGL task
// (lichen_keys.h), so the widget calls below need no extra lock.
#include "esp_log.h"
#include "fonts.h"
#include "hold_exit.h"
#include "key_matrix.h"
#include "lichen_app.h"
#include "lichen_keys.h"
#include "lichen_mem.h"
#include "lichen_ui.h"
#include "lichen_apps.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_U     49 // pixels per key (1u); 13u = 637 of the 648 px content width
#define KEY_H     54
#define KEY_GAP   4
#define ROW_PITCH (KEY_H + KEY_GAP)

#define CODES (KEY_MATRIX_ROWS * KEY_MATRIX_COLS) // indexed by code - 1; the unwired codes keep no cell

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
    if (!t->cell[index]) {
        return;
    }
    uint32_t shape = key_matrix_color((uint8_t)(index + 1));
    uint32_t bg = t->down[index] ? THEME_AMBER : t->seen[index] ? THEME_TEAL : THEME_TILE;
    lv_obj_set_style_bg_color(t->cell[index], lv_color_hex(bg), 0);
    uint32_t fg = t->down[index] || t->seen[index] ? THEME_BG : shape ? shape : THEME_TEXT;
    lv_obj_set_style_text_color(lv_obj_get_child(t->cell[index], 0), lv_color_hex(fg), 0);
}

static void on_raw(lichen_raw_key_t const *ev, void *arg) {
    keytest_t *t = arg;
    if (key_layout_index(ev->code) >= 0) {
        int index        = ev->code - 1;
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

static lichen_app_t const keytest_app; // defined at the end: the owner of the screen state (#181, #283)
static lichen_ctx_t self_ctx; // lichen_register_app's answer, for the gate calls (#230 E1b)

static void on_delete(lv_event_t *e) {
    keytest_t *t = lv_event_get_user_data(e);
    if (t->listening) {
        lichen_keys_raw_release(on_raw, t);
    }
    lv_timer_delete(t->timer);
    lichen_free(self_ctx, t);
}

static lv_obj_t *add_label(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static void add_cell(keytest_t *t, lv_obj_t *grid, key_layout_t const *k) {
    int       index = k->code - 1;
    lv_obj_t *cell  = lv_obj_create(grid);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, k->w * KEY_U / 4 - KEY_GAP, KEY_H);
    lv_obj_set_pos(cell, k->x * KEY_U / 4, k->row * ROW_PITCH);
    lv_obj_set_style_radius(cell, 6, 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(cell, false);
    lv_obj_t *label = lv_label_create(cell);
    char      buf[8];
    lv_label_set_text(label, key_matrix_label(k->code, buf, sizeof(buf)));
    lv_obj_set_style_text_font(label, lichen_font(LICHEN_FONT_SMALL), 0);
    lv_obj_center(label);
    t->cell[index] = cell;
    style_cell(t, index);
}

static void open_keytest(lv_obj_t *screen) {
    keytest_t *t = lichen_alloc(self_ctx, sizeof(*t), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (!t) {
        add_label(screen, "Out of memory", THEME_RED);
        return;
    }
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 10, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scrollable(screen, false);

    lv_obj_t *title = add_label(screen, "Keyboard test", THEME_TEXT);
    lv_obj_set_style_text_font(title, lichen_font(LICHEN_FONT_LARGE), 0);
    t->event_line = add_label(screen, "press any key", THEME_AMBER);
    t->count_line = add_label(screen, "0 key presses", THEME_DIM);

    lv_obj_t *grid = lv_obj_create(screen);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, KEY_LAYOUT_WIDTH * KEY_U / 4, KEY_LAYOUT_ROWS * ROW_PITCH - KEY_GAP);
    key_layout_t const *keys;
    size_t              count = key_layout(&keys);
    for (size_t i = 0; i < count; i++) {
        add_cell(t, grid, &keys[i]);
    }

    add_label(screen, "Hold Esc for 2 s to leave", THEME_DIM);
    t->hold_bar = lv_bar_create(screen);
    lv_obj_set_size(t->hold_bar, LV_PCT(100), 10);
    lv_bar_set_range(t->hold_bar, 0, 100);
    lv_obj_set_style_bg_color(t->hold_bar, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_color(t->hold_bar, lv_color_hex(THEME_AMBER), LV_PART_INDICATOR);

    t->timer     = lv_timer_create(tick, 50, t);
    t->listening = lichen_keys_raw_take(on_raw, t);
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

// A keycap with a K on it
static void icon_keytest(lv_obj_t *parent, int size_px) {
    lv_obj_set_style_radius(parent, size_px / 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_t *cap = lv_obj_create(parent);
    lv_obj_remove_style_all(cap);
    lv_obj_set_size(cap, size_px * 5 / 8, size_px * 5 / 8);
    lv_obj_set_style_radius(cap, size_px / 10, 0);
    lv_obj_set_style_border_width(cap, size_px >= 64 ? 4 : 2, 0);
    lv_obj_set_style_border_color(cap, lv_color_hex(THEME_TEAL), 0);
    lv_obj_center(cap);
    lv_obj_t *k = lv_label_create(cap);
    lv_obj_set_style_text_color(k, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_font(k, size_px >= 64 ? lichen_font(LICHEN_FONT_LARGE) : lichen_font(LICHEN_FONT_SMALL), 0);
    lv_label_set_text(k, "K");
    lv_obj_center(k);
}

static lichen_app_t const keytest_app = {
    .name      = "Keyboard test",
    .id        = "keytest",
    .draw_icon = icon_keytest,
    .tile_text = keytest_tile,
    .open      = open_keytest,
};

void keytest_app_register(void) {
    self_ctx = lichen_register_app(&keytest_app);
}
