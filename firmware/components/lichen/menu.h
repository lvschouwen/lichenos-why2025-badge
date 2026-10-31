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
// Moves the selection to the index-th selectable row (counting menu_add and menu_add_slider rows only)
void      menu_focus_row(lv_obj_t *menu, int index);
// Keys no row uses (printable characters and Backspace) go to fn(key, arg),
// from the key handling of whichever row has the focus. For type-to-search (menu_filter.h).
void      menu_on_text(lv_obj_t *menu, bool (*fn)(uint32_t key, void *arg), void *arg);
