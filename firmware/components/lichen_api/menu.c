// The list menu helper (#66): see menu.h
#include "menu.h"

#include "gate_mem.h"
#include "lichen_ui.h"
#include "lichen_ui_ops.h"
#include "fonts.h"
#include "scroll_anchor.h"
#include "theme.h"
#include "ui_text.h"

#include "esp_log.h"

#include <stdio.h>

#define ROW_PAD_V   14
#define ROW_PAD_H   20
#define ROW_GAP     10
#define SLIDER_W    300
#define SLIDER_STEPS 20
// Share of the row a label may take before its value gets squeezed to nothing (#131): the value keeps the rest. A
// slider row also holds the slider (300 px) and a 64 px value, which leaves about a third of the 602 px content width.
#define LABEL_MAX_PCT        68
#define LABEL_MAX_PCT_SLIDER 34
#define SCROLL_STEP  96
#define CHEVRON      "\xE2\x80\xBA" // U+203A, after the value of a row that opens a page (R3, #135)
#define DANGER_SECS  10

static char const TAG[] = "menu";

// In PSRAM through gate_mem.h (#296), charged to whoever added the row: freed with owner, never lichen_ctx_self()
typedef struct {
    lichen_ctx_t owner;
    void (*on_enter)(void *);
    void (*on_change)(int, void *);
    void     *arg;
    lv_obj_t *value;  // the right-hand label
    lv_obj_t *slider; // slider rows only
    int       min, max;
    menu_kind_t kind;
    lv_obj_t   *chev;   // MENU_OPEN's mark, made when first needed
    lv_group_t *group;  // the group the row joined, to put an INFO row back
    gate_mem_pair_t danger; // MENU_DANGER's dialog: .a the question, .b the confirm label (each may be NULL)
} row_t;

static void apply_kind(lv_obj_t *r, row_t *row);

// The one careful dialog at a time that a DANGER row may have open, with what to run on a yes (#135). The row may be
// gone by the answer, so the action is copied here rather than reached through the row. But arg usually belongs to the
// row's screen: when the row is deleted before the action ran, drop it (on_delete) rather than run it on freed memory
// (#149). row is only compared, never dereferenced.
static struct {
    void (*on_enter)(void *);
    void  *arg;
    row_t *row;
} danger;

static void fire_danger(void *unused) {
    (void)unused;
    void (*fn)(void *) = danger.on_enter;
    void *arg          = danger.arg;
    danger.on_enter    = NULL;
    danger.row         = NULL;
    if (fn) {
        fn(arg);
    }
}

static void danger_drop(row_t *row) {
    if (danger.row == row) {
        lichen_ui_ops_later_cancel(fire_danger, NULL);
        danger.on_enter = NULL;
        danger.row      = NULL;
    }
}

// The answer arrives inside a key handler or timer of the dialog: the action runs later, outside it, like on_enter
static void danger_done(bool confirmed) {
    if (confirmed && lichen_ui_ops_later(fire_danger, NULL)) {
        return;
    }
    danger.on_enter = NULL; // a no, or a yes the UI service could not queue: nothing will run
    danger.row      = NULL;
}

static void ask_danger(row_t *row) {
    danger.on_enter = row->on_enter;
    danger.arg      = row->arg;
    danger.row      = row;
    shell_dialog_t d = {
        .title         = row->danger.a ? row->danger.a : "Are you sure?",
        .hint          = NULL, // generated from confirm_label
        .seconds       = DANGER_SECS,
        .careful       = true,
        .confirm_label = row->danger.b ? row->danger.b : "continue",
        .cancel_label  = "cancel",
    };
    if (!shell_dialog_countdown(&d, danger_done)) {
        danger.on_enter = NULL;
        danger.row      = NULL;
    }
}

static void fire_enter(void *data) {
    row_t *row = data;
    if (row->kind == MENU_DANGER) {
        ask_danger(row); // R3: anything destructive or hard to undo asks first
    } else {
        row->on_enter(row->arg);
    }
}

// The menu's own user data: who gets the keys no row uses (#32), the status line and the empty-state line (#135)
typedef struct {
    bool (*fn)(uint32_t key, void *arg);
    void *arg;
} text_hook_t;

typedef struct {
    lichen_ctx_t owner; // who made the menu (gate_mem.h, #296)
    text_hook_t hook;
    lv_obj_t   *status; // menu_status_*: made by the first begin
    ui_status_t status_state;
    lv_obj_t   *empty;  // menu_empty: gone once a row is added
} menu_t;

static menu_t *menu_data(lv_obj_t *menu) {
    return menu ? lv_obj_get_user_data(menu) : NULL;
}

// A row the keys can reach: INFO rows are shown but never selected (R3)
static bool is_selectable(lv_obj_t *child) {
    row_t *row = lv_obj_get_user_data(child);
    return row && row->kind != MENU_INFO;
}

// No selectable row comes after this one in its menu (hidden rows, which a filter hides, are not selectable)
static bool is_last_row(lv_obj_t *menu, lv_obj_t *row) {
    for (uint32_t i = lv_obj_get_index(row) + 1; i < lv_obj_get_child_count(menu); i++) {
        lv_obj_t *child = lv_obj_get_child(menu, (int32_t)i);
        if (is_selectable(child) && !lv_obj_is_hidden(child)) {
            return false;
        }
    }
    return true;
}

// No selectable row comes before this one (hidden rows don't count)
static bool is_first_row(lv_obj_t *menu, lv_obj_t *row) {
    for (uint32_t i = 0; i < lv_obj_get_index(row); i++) {
        lv_obj_t *child = lv_obj_get_child(menu, (int32_t)i);
        if (is_selectable(child) && !lv_obj_is_hidden(child)) {
            return false;
        }
    }
    return true;
}

void menu_scroll_keep(lv_obj_t *menu, int32_t keep_y, lv_obj_t *focused, lv_anim_enable_t anim) {
    lv_obj_update_layout(menu);
    int32_t from   = lv_obj_get_scroll_y(menu);
    int32_t bottom = lv_obj_get_y(focused) + lv_obj_get_height(focused);
    int32_t view_h = lv_obj_get_height(menu) - lv_obj_get_style_pad_top(menu, LV_PART_MAIN);
    int32_t to     = scroll_keep_top(from, keep_y, bottom, view_h);
    if (to != from) {
        lv_obj_scroll_to_y(menu, to, anim);
    }
}

// The first row brings the heading (and any note above the rows) back into view: scrolling a row just into view
// would leave the title off screen for good once the list had scrolled (#128). LVGL's own scroll-on-focus has run
// by now (class handler first); this replaces its animation only when the target differs.
static void on_focused(lv_event_t *e) {
    lv_obj_t *row  = lv_event_get_target(e);
    lv_obj_t *menu = lv_obj_get_parent(row);
    if (is_first_row(menu, row)) {
        menu_scroll_keep(menu, 0, row, LV_ANIM_ON);
    }
}

static void pass_to_hook(lv_obj_t *obj, uint32_t key) {
    menu_t *m = menu_data(lv_obj_get_parent(obj));
    if (m && m->hook.fn) {
        m->hook.fn(key, m->hook.arg);
    }
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
    } else if (key == LV_KEY_ENTER) {
        // Opening a screen swaps the indev's group: not from inside its key handling. A row a filter hid (it can
        // still hold the focus when nothing matches) does nothing.
        if (row->on_enter && row->kind != MENU_INFO && !lv_obj_is_hidden(obj)) {
            lichen_ui_ops_later(fire_enter, row);
        }
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
    } else if (key != LV_KEY_ESC && key != LV_KEY_NEXT && key != LV_KEY_PREV && key != LV_KEY_LEFT &&
               key != LV_KEY_RIGHT) {
        pass_to_hook(obj, key);
    }
}

// LVGL context, so lichen_ctx_self() is the OS here: each block goes back to the ctx it was charged to
static void on_delete(lv_event_t *e) {
    row_t *row = lv_event_get_user_data(e);
    lichen_ui_ops_later_cancel(fire_enter, row);
    danger_drop(row);
    gate_mem_pair_clear(&row->danger);
    gate_mem_free(row->owner, row);
}

// A menu with no selectable row (a page of only INFO rows: Radio health, a contact's Signal page) takes the focus
// itself: else the last row added had scrolled it to the bottom on its way out of the group, with nothing left for
// Up/Down to move to and the heading out of reach (#174). It opens at the top and Up/Down scroll it.
static bool has_selectable(lv_obj_t *menu) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(menu); i++) {
        if (is_selectable(lv_obj_get_child(menu, (int32_t)i))) {
            return true;
        }
    }
    return false;
}

static void focus_fallback(lv_obj_t *menu, lv_group_t *g) {
    if (!g || !menu_data(menu)) {
        return;
    }
    if (has_selectable(menu)) {
        if (lv_obj_get_group(menu)) {
            lv_group_remove_obj(menu);
        }
        return;
    }
    if (!lv_obj_get_group(menu)) {
        lv_group_add_obj(g, menu);
    }
    lv_group_focus_obj(menu);
    lv_obj_scroll_to_y(menu, 0, LV_ANIM_OFF);
}

static void on_menu_key(lv_event_t *e) {
    lv_obj_t *menu = lv_event_get_current_target(e);
    if (lv_event_get_target(e) != menu) {
        return; // a row's key, handled by on_key
    }
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        shell_scroll_by(menu, key == LV_KEY_UP ? SCROLL_STEP : -SCROLL_STEP);
    } else if (key != LV_KEY_ESC && key != LV_KEY_NEXT && key != LV_KEY_PREV && key != LV_KEY_LEFT &&
               key != LV_KEY_RIGHT && key != LV_KEY_ENTER) {
        menu_t *m = menu_data(menu);
        if (m && m->hook.fn) {
            m->hook.fn(key, m->hook.arg);
        }
    }
}

static void on_menu_delete(lv_event_t *e) {
    menu_t *m = lv_event_get_user_data(e);
    if (m->status_state.state == UI_STATUS_RUNNING) { // R5: a wait must end; say which one did not
        ESP_LOGW(TAG, "status \"%s\" began and never ended", m->status_state.verb);
    }
    gate_mem_free(m->owner, m); // LVGL context: the stored owner, as on_delete
}

static lv_obj_t *menu_base(lv_obj_t *parent) {
    lv_obj_t *menu = lv_obj_create(parent);
    lv_obj_remove_style_all(menu);
    lv_obj_set_size(menu, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(menu, ROW_GAP, 0);
    lv_obj_set_style_pad_bottom(menu, THEME_PAD, 0);
    lv_obj_set_scrollbar_mode(menu, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(menu, LV_DIR_VER);
    shell_scroll_tame(menu); // it scrolls only when the rows don't fit (focus moves it), never past them
    lichen_ctx_t owner = LICHEN_CTX_NONE;
    menu_t      *m     = gate_mem_new(sizeof(*m), &owner);
    if (m) {
        m->owner = owner;
        lv_obj_set_user_data(menu, m);
        lv_obj_add_event_cb(menu, on_menu_delete, LV_EVENT_DELETE, m);
        lv_obj_add_event_cb(menu, on_menu_key, LV_EVENT_KEY, NULL);
    }
    return menu;
}

lv_obj_t *menu_create(lv_obj_t *parent, char const *title) {
    lv_obj_t *menu = menu_base(parent);
    // One line at the content width, "..." when the name does not fit: "Settings: <a repeater's name>" ran off the
    // screen (#131). The font goes on before the line height is taken.
    lv_obj_t *heading = lichen_label_line(menu, LV_PCT(100));
    lv_obj_set_style_text_font(heading, lichen_font(LICHEN_FONT_LARGE), 0);
    lv_obj_set_style_pad_bottom(heading, 8, 0);
    lichen_label_line_fit(heading); // after the font and the padding
    lv_label_set_text(heading, title);
    return menu;
}

lv_obj_t *menu_create_named(lv_obj_t *parent, char const *kind, char const *name) {
    lv_obj_t *menu = menu_base(parent);
    lichen_heading(menu, kind, name); // R1: the kind small and dim, the name LARGE on its own line (#135)
    return menu;
}


void menu_on_text(lv_obj_t *menu, bool (*fn)(uint32_t key, void *arg), void *arg) {
    menu_t *m = menu_data(menu);
    if (m) {
        m->hook.fn  = fn;
        m->hook.arg = arg;
    }
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
    menu_t *m = menu_data(menu);
    if (m && m->empty) { // R11: the empty-state line goes when the first row comes
        lv_obj_delete(m->empty);
        m->empty = NULL;
    }
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
    // The border is always there and only shows when focused: LVGL insets the content by the border width, so a
    // border that comes and goes would shift the row's text by 3 px as the selection moves (#20)
    lv_obj_set_style_border_color(r, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_border_width(r, 3, 0);
    lv_obj_set_style_border_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(r, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(r, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(r, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(r, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 16, 0);
    lv_obj_set_scrollable(r, false);
    lv_obj_set_scroll_on_focus(r, true);

    // The label keeps the width it needs (what the user picks by stays readable); the value takes what is left, on
    // one line, right-aligned, and ends in "..." when it does not fit. Never overlapping, whatever the font (#28)
    int32_t line_h = lv_font_get_line_height(lv_obj_get_style_text_font(r, LV_PART_MAIN));
    lv_obj_t *label = lv_label_create(r);
    row->value      = lv_label_create(r);
    lv_obj_set_style_text_color(row->value, lv_color_hex(THEME_DIM), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(label, value_text && value_text[0] ? LV_PCT(LABEL_MAX_PCT) : LV_PCT(100), 0);
    lv_obj_set_height(label, line_h);
    lv_label_set_long_mode(row->value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(row->value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(row->value, 0);
    lv_obj_set_flex_grow(row->value, 1);
    lv_obj_set_height(row->value, line_h);
    lv_label_set_text(label, label_text);
    lv_label_set_text(row->value, value_text ? value_text : "");
    lv_obj_add_event_cb(r, on_key, LV_EVENT_KEY, row);
    lv_obj_add_event_cb(r, on_focused, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(r, on_delete, LV_EVENT_DELETE, row);
    row->group = lv_group_get_default();
    if (row->group) {
        lv_group_add_obj(row->group, r);
    }
    if (lv_obj_get_group(menu)) { // the menu held the focus (#174): a selectable row takes it back
        lv_group_remove_obj(menu);
    }
    return r;
}

// A zeroed row charged to the caller (gate_mem.h): NULL without room, before anything of the row exists
static row_t *row_new(void) {
    lichen_ctx_t owner = LICHEN_CTX_NONE;
    row_t       *row   = gate_mem_new(sizeof(*row), &owner);
    if (row) {
        row->owner = owner;
    }
    return row;
}

static lv_obj_t *add_plain(lv_obj_t *menu, char const *label, char const *value, void (*on_enter)(void *), void *arg) {
    row_t *row = row_new();
    if (!row) {
        return NULL;
    }
    row->on_enter = on_enter;
    row->arg      = arg;
    lv_obj_t *r   = add_row(menu, label, value, row);
    lv_obj_set_user_data(r, row);
    return r;
}

lv_obj_t *menu_add(lv_obj_t *menu, char const *label, char const *value, void (*on_enter)(void *), void *arg) {
    return add_plain(menu, label, value, on_enter, arg);
}

lv_obj_t *menu_add_slider(lv_obj_t *menu, char const *label, int min, int max, int value,
                          void (*on_change)(int, void *), void *arg) {
    row_t *row = row_new();
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
    lv_obj_set_style_bg_color(row->slider, lv_color_hex(THEME_SLIDER_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(row->slider, lv_color_hex(THEME_AMBER), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(row->slider, lv_color_hex(THEME_AMBER), LV_PART_KNOB);
    lv_obj_set_style_outline_width(row->slider, 0, LV_STATE_ANY);
    lv_obj_set_style_text_align(row->value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(row->value, 64);
    lv_obj_set_style_max_width(lv_obj_get_child(r, 0), LV_PCT(LABEL_MAX_PCT_SLIDER), 0); // else a long label pushes the slider out
    return r;
}

void menu_set_value(lv_obj_t *row, char const *value) {
    row_t *r = row ? lv_obj_get_user_data(row) : NULL;
    if (r) {
        lv_label_set_text(r->value, value);
        if (!r->slider) { // an empty value gives the label the full row, one that appears leaves it room (#131)
            lv_obj_set_style_max_width(lv_obj_get_child(row, 0), value && value[0] ? LV_PCT(LABEL_MAX_PCT) : LV_PCT(100), 0);
        }
    }
}

void menu_set_label(lv_obj_t *row, char const *label) {
    if (row && lv_obj_get_user_data(row)) {
        lv_label_set_text(lv_obj_get_child(row, 0), label); // add_row: the label is the row's first child
    }
}

void menu_focus_row(lv_obj_t *menu, int index) {
    uint32_t n = lv_obj_get_child_count(menu);
    int      k = 0;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(menu, (int32_t)i);
        if (!is_selectable(child)) {
            continue; // the heading, notes and INFO rows
        }
        if (k++ == index) {
            lv_group_focus_obj(child);
            return;
        }
    }
}

// ---- row kinds (R3, #135) ----------------------------------------------------------------------------------------

static void set_chevron(lv_obj_t *r, row_t *row, bool shown) {
    if (shown && !row->chev) {
        row->chev = lv_label_create(r);
        lv_obj_set_style_text_color(row->chev, lv_color_hex(THEME_DIM), 0);
        lv_label_set_text(row->chev, CHEVRON);
    }
    if (row->chev) {
        if (shown) {
            lv_obj_set_hidden(row->chev, false);
        } else {
            lv_obj_set_hidden(row->chev, true);
        }
    }
}

// Each kind is set from scratch, so a row can change kind (a Mount row that becomes a plain one)
static void apply_kind(lv_obj_t *r, row_t *row) {
    lv_obj_t *label = lv_obj_get_child(r, 0);
    set_chevron(r, row, row->kind == MENU_OPEN);
    lv_obj_remove_local_style_prop(label, LV_STYLE_TEXT_COLOR, 0);
    lv_obj_set_style_text_color(row->value, lv_color_hex(THEME_DIM), 0);
    if (row->kind == MENU_CYCLE || row->kind == MENU_TOGGLE) { // the state is what the row is about: not dimmed
        lv_obj_set_style_text_color(row->value, lv_color_hex(THEME_TEXT), 0);
    } else if (row->kind == MENU_DANGER) { // attention colour; the careful dialog is the word behind it
        lv_obj_set_style_text_color(label, lv_color_hex(THEME_AMBER), 0);
    } else if (row->kind == MENU_INFO) {
        lv_obj_set_style_text_color(label, lv_color_hex(THEME_DIM), 0);
    }
    if (row->group) { // INFO leaves the group so Up/Down pass over it; any other kind is in it
        if (row->kind == MENU_INFO) {
            lv_group_remove_obj(r);
        } else if (lv_obj_get_group(r) == NULL) {
            lv_group_add_obj(row->group, r);
        }
        focus_fallback(lv_obj_get_parent(r), row->group);
    }
}

void menu_set_kind(lv_obj_t *row, menu_kind_t kind) {
    row_t *r = row ? lv_obj_get_user_data(row) : NULL;
    if (r) {
        r->kind = kind;
        apply_kind(row, r);
    }
}

void menu_set_on(lv_obj_t *row, bool on) {
    menu_set_value(row, on ? "On" : "Off");
}

void menu_set_danger(lv_obj_t *row, char const *question, char const *confirm_label) {
    row_t *r = row ? lv_obj_get_user_data(row) : NULL;
    if (!r) {
        return;
    }
    // The new words are made before the old go. Without room the row asks the generic "Are you sure?" rather than
    // keeping words that may name another action; it still asks (R3).
    if (!gate_mem_pair_set(&r->danger, question, confirm_label)) {
        ESP_LOGW(TAG, "no room for a danger row's words: it asks \"Are you sure?\"");
        gate_mem_pair_clear(&r->danger);
    }
    menu_set_kind(row, MENU_DANGER);
}

// ---- status line and empty state (R5, R11) -------------------------------------------------------------------------

// The status line sits right under the heading and keeps its place while rows come and go
static lv_obj_t *status_label(lv_obj_t *menu, menu_t *m) {
    if (!m->status) {
        m->status = lichen_label_wrap(menu, LV_PCT(100));
        lv_obj_set_style_pad_top(m->status, 4, 0);
        if (lv_obj_get_child_count(menu) > 1) {
            lv_obj_move_to_index(m->status, 1);
        }
    }
    return m->status;
}

static void status_show(lv_obj_t *menu, menu_t *m, char const *text, uint32_t color) {
    lv_obj_t *label = status_label(menu, m);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
}

void menu_status_begin(lv_obj_t *menu, char const *verb) {
    menu_t *m = menu_data(menu);
    if (!m) {
        return;
    }
    if (ui_status_begin(&m->status_state, verb)) {
        ESP_LOGW(TAG, "status began again before the last one ended");
    }
    char text[64];
    ui_status_begin_text(text, sizeof(text), verb);
    status_show(menu, m, text, THEME_DIM);
}

void menu_status_progress(lv_obj_t *menu, int done, int of) {
    menu_t *m = menu_data(menu);
    if (!m || m->status_state.state != UI_STATUS_RUNNING) {
        return;
    }
    char text[64];
    ui_status_progress_text(text, sizeof(text), m->status_state.verb, done, of);
    status_show(menu, m, text, THEME_DIM);
}

void menu_status_done(lv_obj_t *menu, char const *text) {
    menu_t *m = menu_data(menu);
    if (m && ui_status_end(&m->status_state, false)) {
        status_show(menu, m, text ? text : "", THEME_TEAL);
    }
}

void menu_status_fail(lv_obj_t *menu, char const *why, char const *next) {
    menu_t *m = menu_data(menu);
    if (m && ui_status_end(&m->status_state, true)) {
        char text[192];
        ui_status_fail_text(text, sizeof(text), why, next);
        status_show(menu, m, text, THEME_RED);
    }
}

void menu_empty(lv_obj_t *menu, char const *what, char const *how) {
    menu_t *m = menu_data(menu);
    if (!m) {
        return;
    }
    char text[192];
    ui_empty_text(text, sizeof(text), what, how);
    if (!m->empty) {
        m->empty = lichen_label_wrap(menu, LV_PCT(100));
        lv_obj_set_style_text_color(m->empty, lv_color_hex(THEME_DIM), 0);
        lv_obj_set_style_pad_top(m->empty, 8, 0);
    }
    lv_label_set_text(m->empty, text);
}
