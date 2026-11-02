#pragma once

#include "lvgl.h"

// A list menu for the keyboard (#66): Up/Down move between rows, Enter activates one, Left/Right change a slider row.
// Esc is the shell's. Rows join the LVGL group that is the default while the screen is built (shell_open does that).
// parent is a screen or any container with the room; the menu fills its width.
lv_obj_t *menu_create(lv_obj_t *parent, char const *title);
// A row with a label and a value on the right (NULL for none); on_enter(arg) runs outside the key handling, so it may
// open a screen. Returns the row for menu_set_value().
lv_obj_t *menu_add(lv_obj_t *menu, char const *label, char const *value, void (*on_enter)(void *), void *arg);
// A row with a slider from min to max; Left/Right step by (max - min) / 20 and call on_change(value, arg)
lv_obj_t *menu_add_slider(lv_obj_t *menu, char const *label, int min, int max, int value,
                          void (*on_change)(int, void *), void *arg);
// A dimmed line that cannot be selected: a section heading or a note
lv_obj_t *menu_note(lv_obj_t *menu, char const *text);
void      menu_set_value(lv_obj_t *row, char const *value);
// A row's label, for a row whose action follows a state (Mount / Unmount)
void      menu_set_label(lv_obj_t *row, char const *label);
// Moves the selection to the index-th selectable row (counting menu_add and menu_add_slider rows only)
void      menu_focus_row(lv_obj_t *menu, int index);
// Scrolls the menu up so its content from keep_y down (0: the heading) shows too, when the focused row still fits
// in the view below it; else leaves the scroll alone. The first row does this with 0 by itself.
void      menu_scroll_keep(lv_obj_t *menu, int32_t keep_y, lv_obj_t *focused, lv_anim_enable_t anim);
// Keys no row uses (printable characters and Backspace) go to fn(key, arg),
// from the key handling of whichever row has the focus. For type-to-search (menu_filter.h).
void      menu_on_text(lv_obj_t *menu, bool (*fn)(uint32_t key, void *arg), void *arg);
