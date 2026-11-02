// The station clock screensaver (#119): the shared station view (station_view.c) on the saver screen, a 576 px dial near
// the top with the date and the unread count under it. The view stops and frees itself when the saver screen is deleted.
#include "saver_station.h"

#include "fonts.h"
#include "station_view.h"
#include "theme.h"

#define DIAL_D    576 // the date and the unread count go under it
#define DIAL_TOP  24
#define DATE_GAP  8

bool saver_station_create(lv_obj_t *screen) {
    int32_t line_h = lv_font_get_line_height(lichen_font(LICHEN_FONT_LARGE));
    int32_t line1  = DIAL_TOP + DIAL_D + DATE_GAP;
    station_view_cfg_t cfg = {
        .layout       = {DIAL_D, DIAL_TOP, line1, line1 + line_h},
        .line2_font   = LICHEN_FONT_SMALL,
        .line2_color  = THEME_AMBER,
        .update_line2 = saver_notes_update,
    };
    return station_view_create(screen, &cfg) != NULL;
}
