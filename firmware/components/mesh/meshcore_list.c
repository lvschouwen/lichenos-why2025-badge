// The MeshCore app's list screens (#88): see meshcore_list.h. Moved out of the old two-tab screen (#67, #48, #49).
#include "meshcore_list.h"

#include "fonts.h"
#include "lichen_ui.h"
#include "theme.h"

#define HINT_ROOM 56 // the list's bottom padding: its last row is never under the hint

lv_obj_t *mc_list_label(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

// One line, cut with "...": a row would grow otherwise
lv_obj_t *mc_list_line(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, 0)));
    return label;
}

void mc_list_style_row(lv_obj_t *row, bool selected) {
    lv_obj_set_style_bg_color(row, lv_color_hex(THEME_TILE), 0);
    lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

lv_obj_t *mc_list_row(mc_list_t *l, bool selected) {
    lv_obj_t *row = lv_obj_create(l->list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_ver(row, 4, 0);
    lv_obj_set_style_pad_hor(row, 6, 0);
    lv_obj_set_style_pad_row(row, 2, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, false);
    mc_list_style_row(row, selected);
    return row;
}

static void no_focus_ring(lv_obj_t *obj) {
    lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUS_KEY);
}

lv_obj_t *mc_list_screen(lv_obj_t *screen, char const *title, char const *note, char const *hint) {
    lv_obj_set_style_pad_bottom(screen, 0, 0);
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 8, 0);
    lv_obj_t *heading = lv_label_create(screen);
    lv_obj_set_style_text_font(heading, lichen_font(LICHEN_FONT_LARGE), 0);
    lv_label_set_text(heading, title);
    if (note) {
        mc_list_label(screen, note, THEME_DIM);
    }
    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_style_pad_bottom(list, HINT_ROOM, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
    shell_scroll_tame(list);
    no_focus_ring(list);
    lv_group_t *group = lv_group_get_default();
    if (group) {
        lv_group_add_obj(group, list);
    }
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_floating(label, true); // over the list's bottom padding, not in the column
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_bg_color(label, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(label, 6, 0);
    lv_label_set_text(label, hint);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -8);
    return list;
}

// The rows with their keys: the first n children (a note may follow them); 0 while there are none
static size_t keyed_rows(mc_list_t *l) {
    if (l->n == 0 || lv_obj_get_child_count(l->list) < l->n) {
        return 0;
    }
    for (size_t i = 0; i < l->n; i++) {
        lv_obj_t *row     = lv_obj_get_child(l->list, (int32_t)i);
        l->items[i].top    = lv_obj_get_y(row); // in the content: does not move with scrolling
        l->items[i].bottom = l->items[i].top + lv_obj_get_height(row);
        l->items[i].key    = l->keys[i];
    }
    return l->n;
}

mc_scroll_t mc_list_save(mc_list_t *l) {
    mc_scroll_t s = {.y = lv_obj_get_scroll_y(l->list)};
    s.at_top      = s.y <= 0;
    s.anchor      = scroll_anchor_take(l->items, keyed_rows(l), s.y);
    return s;
}

void mc_list_restore(mc_list_t *l, mc_scroll_t s) {
    lv_obj_update_layout(l->list);
    int32_t y = s.y;
    if (s.at_top) {
        y = 0;
    } else {
        scroll_anchor_find(&s.anchor, l->items, keyed_rows(l), &y);
    }
    // scroll_to_y is bounded: a list that fits doesn't move, a shorter one doesn't end up overscrolled
    lv_obj_scroll_to_y(l->list, y, LV_ANIM_OFF);
}

bool mc_list_move(mc_list_t *l, int delta) {
    if (!l->n || lv_obj_get_child_count(l->list) < l->n) {
        return false;
    }
    size_t to = delta < 0 ? (l->sel > 0 ? l->sel - 1 : 0) : (l->sel + 1 < l->n ? l->sel + 1 : l->sel);
    if (to == l->sel) {
        return false;
    }
    mc_list_style_row(lv_obj_get_child(l->list, (int32_t)l->sel), false);
    l->sel        = to;
    lv_obj_t *row = lv_obj_get_child(l->list, (int32_t)to);
    mc_list_style_row(row, true);
    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    return true;
}
