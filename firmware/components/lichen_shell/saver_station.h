#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>

// The screensaver's styles (#119), u8 in settings_store under IDLE_SAVER_STYLE_KEY (idle_policy.h)
typedef enum { SAVER_STYLE_STATION = 0, SAVER_STYLE_DIGITAL = 1 } saver_style_t;

// The Swiss railway style station clock (saver_station.c, #119), built on the saver screen; LVGL task. false: no PSRAM
// for the dial (nothing was added; the caller shows the seven-segment clock instead). The view stops and frees itself
// when the saver screen is deleted (station_view.h).
bool saver_station_create(lv_obj_t *screen);

// screen_saver.c, for both styles: the unread count under the clock, hidden at none
void saver_notes_update(lv_obj_t *notes);
