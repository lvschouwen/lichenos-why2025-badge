#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>

// A full-screen text entry for the badge keyboard (#66), opened as a shell screen. Characters insert, Backspace and Del
// delete, Left/Right move the cursor, Enter = done, Esc = cancel (done is not called). With secret the text shows as dots
// and Tab toggles showing it (Shift press and release do not reach LVGL, so it is not "while Shift is held").
// done runs from LVGL's context once the entry screen is gone, with a copy of the text that is wiped afterwards: copy
// what you keep, and never log it. max is at most 128 characters.
void text_entry_open(char const *title, char const *initial, bool secret, size_t max,
                     void (*done)(char const *text, void *arg), void *arg);
