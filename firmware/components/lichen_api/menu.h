#pragma once

#include "lvgl.h"

// A list menu for the keyboard (#66): Up/Down move between rows, Enter activates one, Left/Right change a slider row.
// Esc is the shell's. Rows join the LVGL group that is the default while the screen is built (shell_open does that).
// parent is a screen or any container with the room; the menu fills its width.
lv_obj_t *menu_create(lv_obj_t *parent, char const *title);
// The same for a page about one thing (R1, #135): lichen_heading(kind, name), the kind small and dim over the name in
// LARGE on its own line. Use it whenever the heading holds a name the user must read (a repeater, a contact, a folder).
lv_obj_t *menu_create_named(lv_obj_t *parent, char const *kind, char const *name);
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
// Moves the selection to the index-th selectable row (counting menu_add and menu_add_slider rows only, not INFO rows)
void      menu_focus_row(lv_obj_t *menu, int index);
// Scrolls the menu up so its content from keep_y down (0: the heading) shows too, when the focused row still fits
// in the view below it; else leaves the scroll alone. The first row does this with 0 by itself.
void      menu_scroll_keep(lv_obj_t *menu, int32_t keep_y, lv_obj_t *focused, lv_anim_enable_t anim);
// Keys no row uses (printable characters and Backspace) go to fn(key, arg),
// from the key handling of whichever row has the focus. For type-to-search (menu_filter.h).
void      menu_on_text(lv_obj_t *menu, bool (*fn)(uint32_t key, void *arg), void *arg);

// ---- the UI standard's row and page widgets (#135; R2, R3, R5, R6, R11) ----

// What a row does, shown so the user knows before pressing Enter (R3). A row without a kind (MENU_PLAIN) acts at once.
typedef enum {
    MENU_PLAIN = 0,
    MENU_OPEN,   // opens a page: a dim "›" after the value
    MENU_CYCLE,  // Enter steps to the next value (the app does it and calls menu_set_value); the value is shown undimmed
    MENU_TOGGLE, // On/Off (menu_set_on); the value is shown undimmed
    MENU_DANGER, // destructive or hard to undo: Enter first asks in the careful dialog (menu_set_danger), on_enter runs on a yes
    MENU_INFO,   // read-only: dim, never selected, Enter does nothing
} menu_kind_t;
void menu_set_kind(lv_obj_t *row, menu_kind_t kind);
// A TOGGLE row's value: "On" or "Off"
void menu_set_on(lv_obj_t *row, bool on);
// Makes the row a DANGER row: Enter shows question with a 10 s careful countdown ("Enter: <confirm_label> \xE2\x80\xA2 Esc:
// cancel"); on_enter runs only when confirmed. The strings are copied. NULL question: "Are you sure?", NULL confirm_label: "continue".
void menu_set_danger(lv_obj_t *row, char const *question, char const *confirm_label);
// Like menu_add, for a long identifying label (R2): the label wraps over the full row width and the value drops under
// it (dim, wrapping too) instead of sharing the line. No value, no second line.
lv_obj_t *menu_add_wrap(lv_obj_t *menu, char const *label, char const *value, void (*on_enter)(void *), void *arg);

// Focus survives a refresh (R6): give rows that exist across rebuilds a key (an id that is not their index, 0 for none),
// save before tearing the rows down, restore after adding them. The row with the saved key gets the focus; if it is
// gone, the row at the saved index (or the last one). INFO rows are not selectable and so not counted.
typedef struct {
    uint32_t key;
    int      index;
} menu_focus_t;
void         menu_set_key(lv_obj_t *row, uint32_t key);
menu_focus_t menu_focus_save(lv_obj_t *menu);
void         menu_focus_restore(lv_obj_t *menu, menu_focus_t f);

// A status line under the heading that ends (R5). begin shows "<verb>...", progress "<verb> 3 of 17", then exactly one
// of done(text) (teal) or fail(why, next) (red, "<why>. <next>" in plain words). A begin that is never ended is logged
// (warning) when the menu goes away or the next begin comes. progress/done/fail without a begin do nothing.
void menu_status_begin(lv_obj_t *menu, char const *verb);
void menu_status_progress(lv_obj_t *menu, int done, int of);
void menu_status_done(lv_obj_t *menu, char const *text);
void menu_status_fail(lv_obj_t *menu, char const *why, char const *next);

// An empty list explains itself (R11): "No <what> yet: <how>." as a dim line, replaced when the first row is added.
// what is a plural noun ("contacts"), how says where they come from or how to get one (NULL for none).
void menu_empty(lv_obj_t *menu, char const *what, char const *how);
