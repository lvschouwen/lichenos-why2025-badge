// Layout probe (#522, tools/check_lv_layout.py): the sizes and offsets of the types an SD app shares with the firmware
// by value or by pointer. The checker compiles this once with the firmware's flags and once with an SD app's; the two
// arrays must be equal, or the app's LVGL (configured by a forced include of the firmware's CONFIG_LV_* values,
// sdapps/sdapp.cmake) reads the firmware's structs at the wrong offsets. LVGL's own opaque types (lv_obj_t) are not
// complete in the public headers, so they are covered through what an app can see: areas, points, colours, styles and
// the enums. Add a line here when an app header starts passing another type.
#include "lichen_app.h"
#include "lichen_notify.h"
#include "lvgl.h"

#include <stddef.h>

#define L(x) (unsigned)(x)
const unsigned lv_layout[] = {
    L(sizeof(lv_area_t)),         L(offsetof(lv_area_t, y2)),
    L(sizeof(lv_point_t)),        L(sizeof(lv_point_precise_t)),
    L(sizeof(lv_color_t)),        L(offsetof(lv_color_t, green)),
    L(sizeof(lv_color32_t)),      L(offsetof(lv_color32_t, alpha)),
    L(sizeof(lv_style_t)),        L(sizeof(lv_style_prop_t)),
    L(sizeof(lv_opa_t)),          L(sizeof(lv_state_t)),
    L(sizeof(lv_obj_flag_t)),     L(sizeof(lv_event_code_t)),
    L(sizeof(lv_align_t)),        L(sizeof(lv_text_align_t)),
    L(sizeof(lichen_app_t)),      L(offsetof(lichen_app_t, open)),
};
