// The list menu helper (#66): see menu.h
#include "menu.h"

#include "shell.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>

#define ROW_PAD_V   14
#define ROW_PAD_H   20
#define ROW_GAP     10
#define SLIDER_W    300
#define SLIDER_STEPS 20
#define SCROLL_STEP  96

typedef struct {
    void (*on_enter)(void *);
    void (*on_change)(int, void *);
    void     *arg;
    lv_obj_t *value;  // the right-hand label
    lv_obj_t *slider; // slider rows only
    int       min, max;
} row_t;

static void fire_enter(void *data) {
    row_t *row = data;
    row->on_enter(row->arg);
}

// No selectable row comes after this one in its menu
static bool is_last_row(lv_obj_t *menu, lv_obj_t *row) {
    for (uint32_t i = lv_obj_get_index(row) + 1; i < lv_obj_get_child_count(menu); i++) {
        if (lv_obj_get_user_data(lv_obj_get_child(menu, (int32_t)i))) {
            return false;
        }
    }
    return true;
}

static void on_key(lv_event_t *e) {
    row_t     *row = lv_event_get_user_data(e);
    lv_obj_t  *obj = lv_event_get_target(e);
    lv_group_t *g  = lv_obj_get_group(obj);
    uint32_t   key = lv_event_get_key(e);
    // A keypad indev hands arrows to the focused object, so moving the selection is up to us
    if (key == LV_KEY_UP) {
        lv_group_focus_prev(g);
    } else if (key == LV_KEY_DOWN) {
        // Text after the last row (an app's about section) can't take the focus: Down scrolls to it first, and only
        // wraps to the top once the menu is at its end
        lv_obj_t *menu = lv_obj_get_parent(obj);
        if (is_last_row(menu, obj) && lv_obj_get_scroll_bottom(menu) > 0) {
            shell_scroll_by(menu, -SCROLL_STEP);
        } else {
            lv_group_focus_next(g);
        }
    } else if (key == LV_KEY_ENTER && row->on_enter) {
        // Opening a screen swaps the indev's group: not from inside its key handling
        lv_async_call(fire_enter, row);
    } else if ((key == LV_KEY_LEFT || key == LV_KEY_RIGHT) && row->slider) {
        int step = (row->max - row->min) / SLIDER_STEPS;
        int v    = lv_slider_get_value(row->slider) + (key == LV_KEY_RIGHT ? step : -step);
        v        = v < row->min ? row->min : v > row->max ? row->max : v;
        if (v != lv_slider_get_value(row->slider)) {
            lv_slider_set_value(row->slider, v, LV_ANIM_OFF);
            char text[8];
            snprintf(text, sizeof(text), "%d", v);
            lv_label_set_text(row->value, text);
            if (row->on_change) {
                row->on_change(v, row->arg);
            }
        }
    }
}

static void on_delete(lv_event_t *e) {
    row_t *row = lv_event_get_user_data(e);
    lv_async_call_cancel(fire_enter, row);
    free(row);
}

lv_obj_t *menu_create(lv_obj_t *parent, char const *title) {
    lv_obj_t *menu = lv_obj_create(parent);
    lv_obj_remove_style_all(menu);
    lv_obj_set_size(menu, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(menu, ROW_GAP, 0);
    lv_obj_set_style_pad_bottom(menu, THEME_PAD, 0);
    lv_obj_set_scrollbar_mode(menu, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(menu, LV_DIR_VER);
    shell_scroll_tame(menu); // it scrolls only when the rows don't fit (focus moves it), never past them
    lv_obj_t *heading = lv_label_create(menu);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_40, 0);
    lv_obj_set_style_pad_bottom(heading, 8, 0);
    lv_label_set_text(heading, title);
    return menu;
}

lv_obj_t *menu_note(lv_obj_t *menu, char const *text) {
    lv_obj_t *label = lv_label_create(menu);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_DIM), 0);
    lv_obj_set_style_pad_top(label, 8, 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *add_row(lv_obj_t *menu, char const *label_text, char const *value_text, row_t *row) {
    lv_obj_t *r = lv_obj_create(menu);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(r, ROW_PAD_H, 0);
    lv_obj_set_style_pad_ver(r, ROW_PAD_V, 0);
    lv_obj_set_style_radius(r, 14, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(THEME_TILE), 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(THEME_TRACK), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(r, lv_color_hex(THEME_TRACK), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(r, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_border_width(r, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(r, 3, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(r, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(r, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 16, 0);
    lv_obj_set_scrollable(r, false);
    lv_obj_set_scroll_on_focus(r, true);

    lv_obj_t *label = lv_label_create(r);
    lv_label_set_text(label, label_text);
    row->value = lv_label_create(r);
    lv_obj_set_style_text_color(row->value, lv_color_hex(THEME_DIM), 0);
    lv_label_set_long_mode(row->value, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(row->value, 0);
    lv_label_set_text(row->value, value_text ? value_text : "");
    lv_obj_add_event_cb(r, on_key, LV_EVENT_KEY, row);
    lv_obj_add_event_cb(r, on_delete, LV_EVENT_DELETE, row);
    lv_group_t *g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, r);
    }
    return r;
}

lv_obj_t *menu_add(lv_obj_t *menu, char const *label, char const *value, void (*on_enter)(void *), void *arg) {
    row_t *row = calloc(1, sizeof(*row));
    if (!row) {
        return NULL;
    }
    row->on_enter = on_enter;
    row->arg      = arg;
    lv_obj_t *r   = add_row(menu, label, value, row);
    lv_obj_set_user_data(r, row);
    return r;
}

lv_obj_t *menu_add_slider(lv_obj_t *menu, char const *label, int min, int max, int value,
                          void (*on_change)(int, void *), void *arg) {
    row_t *row = calloc(1, sizeof(*row));
    if (!row) {
        return NULL;
    }
    row->on_change = on_change;
    row->arg       = arg;
    row->min       = min;
    row->max       = max;
    char text[8];
    snprintf(text, sizeof(text), "%d", value);
    lv_obj_t *r = add_row(menu, label, text, row);
    lv_obj_set_user_data(r, row);
    // The slider sits between label and value; the row has the focus, the slider is only drawn
    row->slider = lv_slider_create(r);
    lv_obj_set_size(row->slider, SLIDER_W, 14);
    lv_obj_set_flex_grow(row->slider, 0);
    lv_obj_move_to_index(row->slider, 1);
    // A slider joins the default group by itself and would take Up/Down as value steps: only the row is selectable
    lv_group_remove_obj(row->slider);
    lv_obj_set_clickable(row->slider, false);
    lv_slider_set_range(row->slider, min, max);
    lv_slider_set_value(row->slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(row->slider, lv_color_hex(THEME_FAINT), LV_PART_MAIN);
    lv_obj_set_style_bg_color(row->slider, lv_color_hex(THEME_AMBER), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(row->slider, lv_color_hex(THEME_AMBER), LV_PART_KNOB);
    lv_obj_set_style_outline_width(row->slider, 0, LV_STATE_ANY);
    lv_obj_set_style_text_align(row->value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(row->value, 64);
    return r;
}

void menu_set_value(lv_obj_t *row, char const *value) {
    row_t *r = row ? lv_obj_get_user_data(row) : NULL;
    if (r) {
        lv_label_set_text(r->value, value);
    }
}

void menu_focus_row(lv_obj_t *menu, int index) {
    uint32_t n = lv_obj_get_child_count(menu);
    int      k = 0;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(menu, (int32_t)i);
        if (!lv_obj_get_user_data(child)) {
            continue; // the heading and notes
        }
        if (k++ == index) {
            lv_group_focus_obj(child);
            return;
        }
    }
}
