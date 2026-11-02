#pragma once

#include "lvgl.h"

#include <stdbool.h>

// Release Notes (#13). notes_app.c: the Release Notes app (a home tile that lists the notes compiled into the image,
// newest first, a badge counting the unread ones) and the "This is new" screen after an update.
void notes_app_register(void); // from shell_start, with the other apps

// Called once after boot by update_screen_boot_check(): updated says this boot follows a successful update. Decides by
// rn_boot_decision() whether to show "This is new" (once everything else on screen is out of the way) or to quietly mark
// the notes seen on a freshly flashed badge, and sets the tile badge.
void notes_boot_check(bool updated);

// notes_render.c: the markdown subset of release_notes.h as LVGL labels in parent (a column container)
void notes_render(lv_obj_t *parent, char const *text, char const *date);
