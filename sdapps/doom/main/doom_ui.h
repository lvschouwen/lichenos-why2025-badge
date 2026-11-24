#pragma once
// The DOOM app's screen (#526): the loading bar, the picture (an LVGL canvas, 2x of the engine's 320x200 frame), the raw keys
// and the fps label. Which task may call what is on each line. GPL-3.0-or-later, like the app as a whole.
#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// WAD size in bytes, for the loading bar's denominator (main(), before the engine starts)
void doom_ui_set_wad_size(uint32_t bytes);
// LVGL task (the open hook): builds the loading state and the hidden canvas, takes the raw keys. False when the picture's
// memory is not there: the screen then says so and the engine runs without a picture.
bool doom_ui_open(lv_obj_t *screen);
// Engine task, from the file shim: n bytes of the WAD were read (the loading bar)
void doom_ui_note_read(size_t n);
// Engine task, DG_DrawFrame: expands the frame into the canvas's buffer and tells the screen (no LVGL call, no lock: the
// screen's timer picks the frame up, so a render in progress never holds the engine)
void doom_ui_present(void);
// Engine task, DG_GetKey
bool doom_ui_next_key(int *pressed, unsigned char *key);
// The loader's stop, after the engine task has ended: takes the keys and the canvas off the LVGL task, then frees the
// picture's memory. False (nothing freed) when the LVGL task did not do it in time.
bool doom_ui_detach(void);
// The final line for the exit notification, "" when no second was played. After doom_ui_detach.
void doom_ui_summary(char *out, size_t cap);
