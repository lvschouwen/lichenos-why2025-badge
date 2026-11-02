#pragma once

#include "lvgl.h"
#include "station_clock.h"

#include <stdbool.h>
#include <stddef.h>

// The Swiss railway style station clock as an LVGL view (#119, #122), shared by the screensaver (saver_station.c) and the
// Clock app (app_clock.c). Both can be up at once (the saver over the app), so each has its own instance. LVGL task only.
typedef struct station_view station_view_t;

typedef struct {
    sc_layout_t layout;        // the dial's size and place and the two lines under it, relative to the parent's content
    int         line2_font;    // a LICHEN_FONT_* for the second line (the first, the date, is LICHEN_FONT_LARGE)
    uint32_t    line2_color;   // THEME_*
    // Called once a second on the LVGL task to set the second line's text (an lv_label); may be NULL
    void (*update_line2)(lv_obj_t *line2);
    // parent is a screen: pause the sweep while another screen is loaded over it (the Clock app under the saver)
    bool pause_when_hidden;
} station_view_cfg_t;

// The date line ("~ " first when the time is unverified); false and "Clock not set" when the clock is not set
bool saver_date_text(char *out, size_t cap);

// Adds the dial (a canvas in PSRAM), the date line and the second line to parent and starts the ~15 fps timer. NULL when
// there is no PSRAM for the dial (nothing was added). The view frees itself, after the parent screen is deleted (the
// timer is deleted first, with the dial; the buffer a moment later, outside the delete).
station_view_t *station_view_create(lv_obj_t *parent, station_view_cfg_t const *cfg);
// The second line's label (for a caller that wants to set it outside update_line2)
lv_obj_t *station_view_line2(station_view_t const *v);
