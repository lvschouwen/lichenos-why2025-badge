#pragma once

#include "lvgl.h"

// Home icons of the built-in apps (app_icons.c)
void icon_settings(lv_obj_t *parent, int size_px);
void icon_about(lv_obj_t *parent, int size_px);
void icon_bench(lv_obj_t *parent, int size_px);
void icon_stress(lv_obj_t *parent, int size_px); // Stress (#278)
void icon_crashlog(lv_obj_t *parent, int size_px);
void icon_notify(lv_obj_t *parent, int size_px);
void icon_power(lv_obj_t *parent, int size_px);
void icon_notes(lv_obj_t *parent, int size_px); // Release notes (#13)
void icon_files(lv_obj_t *parent, int size_px);
