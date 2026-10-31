// Type-to-search on a menu (#32, #57): see menu_filter.h
#include "menu_filter.h"

#include "list_filter.h"
#include "menu.h"
#include "shell.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROWS 128

struct menu_filter {
    lv_obj_t            *menu, *line, *none;
    lv_obj_t            *fallback;
    menu_filter_match_fn match;
    void                *ctx;
    char                 query[LIST_FILTER_MAX + 1];
    size_t               n;
    struct {
        lv_obj_t *row;
        size_t    item;
    } rows[MAX_ROWS];
};

static void show_line(menu_filter_t *f, size_t shown) {
    char text[96];
    if (f->query[0]) {
        snprintf(text, sizeof(text), "Search: %s_  (%u of %u)", f->query, (unsigned)shown, (unsigned)f->n);
    } else {
        snprintf(text, sizeof(text), "Type to search...");
    }
    lv_label_set_text(f->line, text);
    lv_obj_set_style_text_color(f->line, lv_color_hex(f->query[0] ? THEME_AMBER : THEME_DIM), 0);
}

// The focus when its row was hidden: the first row still showing, else the fallback, else it stays (hidden, so nothing
// is highlighted and Enter does nothing)
static lv_obj_t *refocus(menu_filter_t *f, lv_obj_t *focused) {
    if (!focused || !lv_obj_has_flag(focused, LV_OBJ_FLAG_HIDDEN)) {
        return focused;
    }
    for (size_t i = 0; i < f->n; i++) {
        if (!lv_obj_has_flag(f->rows[i].row, LV_OBJ_FLAG_HIDDEN)) {
            lv_group_focus_obj(f->rows[i].row);
            return f->rows[i].row;
        }
    }
    if (f->fallback) {
        lv_group_focus_obj(f->fallback);
        return f->fallback;
    }
    return focused;
}

static void set_hidden(lv_obj_t *obj, bool hidden) {
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply(menu_filter_t *f) {
    size_t shown = 0;
    for (size_t i = 0; i < f->n; i++) {
        bool ok = !f->query[0] || f->match(f->rows[i].item, f->query, f->ctx);
        set_hidden(f->rows[i].row, !ok);
        shown += ok;
    }
    show_line(f, shown);
    set_hidden(f->none, !(f->query[0] && shown == 0 && f->n > 0));
    lv_group_t *g       = f->n ? lv_obj_get_group(f->rows[0].row) : NULL;
    lv_obj_t   *focused = g ? lv_group_get_focused(g) : NULL;
    bool        was_hidden = focused && lv_obj_has_flag(focused, LV_OBJ_FLAG_HIDDEN);
    focused = refocus(f, focused);
    lv_obj_update_layout(f->menu);
    if (focused && !lv_obj_has_flag(focused, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_scroll_to_view(focused, LV_ANIM_OFF);
        if (was_hidden && focused != f->fallback) {
            lv_obj_scroll_to_y(f->menu, 0, LV_ANIM_OFF); // a list that just shrank starts at its top
        }
    } else {
        lv_obj_scroll_to_y(f->menu, 0, LV_ANIM_OFF);
    }
}

static bool on_text(uint32_t key, void *arg) {
    menu_filter_t *f = arg;
    if (!list_filter_edit(f->query, sizeof(f->query), key)) {
        return false;
    }
    apply(f);
    return true;
}

static bool on_esc(void *arg) {
    menu_filter_t *f = arg;
    if (!f->query[0]) {
        return false;
    }
    f->query[0] = '\0';
    apply(f);
    return true;
}

static void on_screen_delete(lv_event_t *e) {
    free(lv_event_get_user_data(e));
}

menu_filter_t *menu_filter_create(lv_obj_t *screen, lv_obj_t *menu, menu_filter_match_fn match, void *ctx) {
    menu_filter_t *f = calloc(1, sizeof(*f));
    if (!f) {
        return NULL;
    }
    f->menu  = menu;
    f->match = match;
    f->ctx   = ctx;
    f->line  = menu_note(menu, "");
    f->none  = menu_note(menu, "Nothing matches. Backspace edits, Esc clears.");
    lv_obj_add_flag(f->none, LV_OBJ_FLAG_HIDDEN);
    show_line(f, 0);
    menu_on_text(menu, on_text, f);
    shell_on_esc(on_esc, f);
    lv_obj_add_event_cb(screen, on_screen_delete, LV_EVENT_DELETE, f);
    return f;
}

void menu_filter_add(menu_filter_t *f, lv_obj_t *row, size_t item) {
    if (!f || !row || f->n >= MAX_ROWS) {
        return;
    }
    f->rows[f->n].row  = row;
    f->rows[f->n].item = item;
    f->n++;
}

void menu_filter_fallback(menu_filter_t *f, lv_obj_t *row) {
    if (f) {
        f->fallback = row;
    }
}
