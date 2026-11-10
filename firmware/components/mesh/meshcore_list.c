// The MeshCore app's list screens (#88): see meshcore_list.h. Moved out of the old two-tab screen (#67, #48, #49).
#include "meshcore_list.h"

#include "fonts.h"
#include "lichen_ui.h"
#include "theme.h"

lv_obj_t *mc_list_label(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lichen_label_wrap(parent, LV_PCT(100));
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

// One line, cut with "...": a row would grow otherwise. As wide as its text until the caller sets a width.
lv_obj_t *mc_list_line(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lichen_label_line(parent, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

#define SEL_BORDER_W 4 // on every row, shown only on the selected one: a border that came and went would move the text

// The selected row (#101): THEME_TRACK with an amber left border, as the chat's search hit (THEME_TILE was too faint)
void mc_list_style_row(lv_obj_t *row, bool selected) {
    lv_obj_set_style_bg_color(row, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(row, SEL_BORDER_W, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_border_opa(row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    // The list holds the group focus, not the row: this state is how `fw ui` finds the highlighted row (#145)
    if (selected) {
        lv_obj_add_state(row, LV_STATE_USER_1);
    } else {
        lv_obj_remove_state(row, LV_STATE_USER_1);
    }
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

lv_obj_t *mc_list_screen(lv_obj_t *screen, char const *title, char const *note, char const *hint_key, char const *hint_verb) {
    lv_obj_set_style_pad_bottom(screen, 0, 0);
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 8, 0);
    lv_obj_t *heading = lichen_label_line(screen, LV_PCT(100)); // a name in the title must not push past the screen (#132)
    lv_obj_set_style_text_font(heading, lichen_font(LICHEN_FONT_LARGE), 0);
    lichen_label_line_fit(heading);
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
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
    shell_scroll_tame(list);
    // The arrows move the highlight (mc_list_move scrolls its row into view). LVGL's own arrow scrolling (on by
    // default) would first scroll the list a quarter of its height on every arrow, Left/Right too, so the highlight
    // crept to the edge and could leave the view (#127, as home's #124)
    lv_obj_set_scroll_with_arrow(list, false);
    no_focus_ring(list);
    lv_group_t *group = lv_group_get_default();
    if (group) {
        lv_group_add_obj(group, list);
    }
    shell_page_hint(screen, hint_key, hint_verb, NULL); // dim, pinned; the screen keeps the room, so no row is under it
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
    int32_t y    = s.y;
    size_t  rows = keyed_rows(l);
    if (s.at_top) {
        y = 0;
    } else {
        scroll_anchor_find(&s.anchor, l->items, rows, &y);
    }
    // scroll_to_y is bounded: a list that fits doesn't move, a shorter one doesn't end up overscrolled
    lv_obj_scroll_to_y(l->list, y, LV_ANIM_OFF);
    // The highlight follows its contact or chat, which may have moved out of the kept view (a re-advert to the top of
    // Heard, a message to the top of the Chat list): bring it back, the least scroll that shows it, none while it is
    // in view. Enter must never open a row nobody can see (#127).
    if (l->sel < rows) {
        lv_obj_scroll_to_view(lv_obj_get_child(l->list, (int32_t)l->sel), LV_ANIM_OFF);
    }
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
