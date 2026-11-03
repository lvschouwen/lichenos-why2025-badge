#pragma once

#include "lvgl.h"

// Home icons of the built-in apps (app_icons.c)
void icon_settings(lv_obj_t *parent, int size_px);
void icon_about(lv_obj_t *parent, int size_px);
void icon_keytest(lv_obj_t *parent, int size_px);
void icon_bench(lv_obj_t *parent, int size_px);
void icon_crashlog(lv_obj_t *parent, int size_px);
void icon_notify(lv_obj_t *parent, int size_px);
void icon_power(lv_obj_t *parent, int size_px);
void icon_notes(lv_obj_t *parent, int size_px); // Release notes (#13)
void icon_files(lv_obj_t *parent, int size_px);
void icon_clock(lv_obj_t *parent, int size_px); // Clock (#122)
void icon_weather(lv_obj_t *parent, int size_px); // Weather (#155), weather_screen.c
void icon_timer(lv_obj_t *parent, int size_px); // Timer (#152)
void icon_calc(lv_obj_t *parent, int size_px);  // Calculator (#158)
