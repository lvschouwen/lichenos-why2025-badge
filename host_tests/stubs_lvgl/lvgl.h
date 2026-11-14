#pragma once
// Host stub of lvgl.h for tests of gate code that only passes LVGL objects along (ui_ops_test.c, #221): opaque types,
// no LVGL call. Kept out of stubs/ so no other test picks it up by accident.
#include <stdint.h>

typedef struct _lv_obj_t   lv_obj_t;
typedef struct _lv_group_t lv_group_t;
// lichen_ui.h's event and timer registration (#349)
typedef struct _lv_event_t     lv_event_t;
typedef struct _lv_event_dsc_t lv_event_dsc_t;
typedef struct _lv_timer_t     lv_timer_t;
typedef int                    lv_event_code_t;
typedef void (*lv_event_cb_t)(lv_event_t *e);
typedef void (*lv_timer_cb_t)(lv_timer_t *t);
