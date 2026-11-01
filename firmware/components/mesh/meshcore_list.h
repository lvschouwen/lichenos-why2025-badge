#pragma once

#include "lvgl.h"
#include "scroll_anchor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The MeshCore app's list screens (#88: Chat, Contacts, Repeaters, Heard): their layout, rows, highlight and the
// scroll position kept across rebuilds. LVGL task only.

// One list on screen. keys[i] is the identity of the list's child i while it shows rows; items is scratch for
// scroll_anchor.c. Both hold cap entries and live in PSRAM with the screen's copies.
typedef struct {
    lv_obj_t      *list;
    uint32_t      *keys;
    scroll_item_t *items;
    size_t         cap;
    size_t         n;   // rows on screen: the list's first n children (a dim note may follow them)
    size_t         sel; // the highlighted row
} mc_list_t;

typedef struct {
    int32_t         y;
    bool            at_top;
    scroll_anchor_t anchor;
} mc_scroll_t;

// The screen of a list: heading, an optional dim note under it (NULL for none), the list (returned, focusable, in the
// default group so it gets the keys) and the key hint at the bottom
lv_obj_t *mc_list_screen(lv_obj_t *screen, char const *title, char const *note, char const *hint);

// A label wrapped to the width of its parent / cut to one line with "..."
lv_obj_t *mc_list_label(lv_obj_t *parent, char const *text, uint32_t color);
lv_obj_t *mc_list_line(lv_obj_t *parent, char const *text, uint32_t color);
// A row: a column of labels, highlighted when selected
lv_obj_t *mc_list_row(mc_list_t *l, bool selected);
void      mc_list_style_row(lv_obj_t *row, bool selected);

// Around a rebuild: take where the reader was before lv_obj_clean, then put them back on the same row (by key) or at
// the top if they were there (a pixel offset jumped a row when the order changed)
mc_scroll_t mc_list_save(mc_list_t *l);
void        mc_list_restore(mc_list_t *l, mc_scroll_t s);

// Up/Down: the highlight moves one row (clamped) and its row scrolls into view; false when nothing moved
bool mc_list_move(mc_list_t *l, int delta);
