#pragma once
// Settings > Apps (#233): the OS's rows on an app's Settings page (what it may use, its data, Clear data, the Background
// switch, its crash count). The page is app_settings.c's; the choices and texts are app_page_model.h's.

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>

// Is the app at index listed under Settings > Apps? (every app but the OS's own without a page of their own)
bool app_page_listed(size_t index);
// Adds the rows below the app's own settings and about rows. LVGL task, the page's screen and menu just built; the rows
// live and die with the screen.
void app_page_add_rows(lv_obj_t *screen, lv_obj_t *menu, size_t index);
