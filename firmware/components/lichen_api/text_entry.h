#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>

// A full-screen text entry for the badge keyboard (#66), opened as a shell screen. Characters insert, Backspace and Del
// delete, Left/Right move the cursor, Enter = done, Esc = cancel (done is not called). With secret the text shows as dots
// and Tab toggles showing it (Shift press and release do not reach LVGL, so it is not "while Shift is held").
// done runs from LVGL's context once the entry screen is gone, with a copy of the text that is wiped afterwards: copy
// what you keep, and never log it. max is at most 128 characters.
// Set on a secret entry's text area for its whole life, whether the text is shown or hidden (Tab), so callers such as
// the key log can tell a secret field without looking at the password mode
#define TEXT_ENTRY_SECRET_BIT 0 // lv_obj_set_user_flag() index

// True when every change to the text since this entry opened came from a hand on the keyboard (#405): a character,
// Backspace or Del from an injected key ("fw keys") or of no known cause marks it false until the next
// text_entry_open, whatever is typed after. An owner whose saved value must be a hand's reads it in done.
bool text_entry_typed_by_hand(void);

// Returns false, and calls nothing, when an entry is already open (there is one at a time); true once it is opening.
bool text_entry_open(char const *title, char const *initial, bool secret, size_t max,
                     void (*done)(char const *text, void *arg), void *arg);
