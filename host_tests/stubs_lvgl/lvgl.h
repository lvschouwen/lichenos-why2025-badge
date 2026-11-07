#pragma once
// Host stub of lvgl.h for tests of gate code that only passes LVGL objects along (ui_ops_test.c, #221): opaque types,
// no LVGL call. Kept out of stubs/ so no other test picks it up by accident.
#include <stdint.h>

typedef struct _lv_obj_t   lv_obj_t;
typedef struct _lv_group_t lv_group_t;
