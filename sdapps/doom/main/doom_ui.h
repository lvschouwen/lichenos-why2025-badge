#pragma once
// The DOOM app's screen (#526, #564): the loading bar, the picture (the OS video surface, 320x200 through a palette LUT, or
// when it is refused an LVGL canvas, 2x of the engine's frame), the raw keys and the fps label. Which task may call what is on each line. GPL-3.0-or-later, like the app as a whole.
#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// main(), before the engine task starts: the key ring and the WAD size in bytes (the loading bar's denominator)
void doom_ui_init(uint32_t wad_bytes);
// LVGL task (the open hook): builds the loading state, takes the raw keys. The picture's path is chosen when the first frame
// comes (the canvas memory is allocated only then, and only when the OS refuses the video surface). False: no keys or timer.
bool doom_ui_open(lv_obj_t *screen);
// Engine task, from the file shim: n bytes of the WAD were read (the loading bar)
void doom_ui_note_read(size_t n);
// Engine task, DG_DrawFrame: expands the frame into the buffer LVGL is not showing and tells the screen (no LVGL call, no
// lock: the screen's timer swaps it in). Drops the frame when the screen has not taken the previous one.
void doom_ui_present(void);
// Engine task, once per tick before doomgeneric_Tick: true while the OS has the screen back (a dialog, the thermal warning):
// the engine waits and draws nothing, which is what lets the screen release the video buffers
bool doom_ui_engine_held(void);
// Engine task, DG_GetKey
bool doom_ui_next_key(int *pressed, unsigned char *key);
// The loader's stop, after the engine task has ended: takes the keys and the canvas off the LVGL task, then frees the
// picture's memory. False (nothing freed) when the LVGL task did not do it within wait_ms.
bool doom_ui_detach(uint32_t wait_ms);
// The final line for the exit notification, "" when no second was played. After doom_ui_detach.
void doom_ui_summary(char *out, size_t cap);
