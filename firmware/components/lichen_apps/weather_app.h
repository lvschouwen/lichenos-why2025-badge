#pragma once
// Weather (#155): the app's shared state. weather_app.c is the background sampler (a ring of one-minute samples in
// PSRAM, the optional CSV log on the SD card) and the Settings page; weather_screen.c the screen. LVGL task for the
// screen calls, any task for the data calls.

#include "lvgl.h"
#include "weather_series.h"

#include <stdbool.h>
#include <stddef.h>

// Quantity q of the history, oldest first, the temperature offset applied; count of values
size_t   weather_series(wx_qty_t q, float *out, size_t max);
// Bumps with every new sample, so the screen redraws only when there is something new
unsigned weather_version(void);
// The temperature offset in kelvin (-10..0)
float    weather_offset_k(void);
// The sensors' latest readings now (not the minute sample), offset applied; false when there is no environment reading
bool     weather_current(wx_sample_t *out);

void weather_screen_open(lv_obj_t *screen); // weather_screen.c: lichen_app_t.open
void icon_weather(lv_obj_t *parent, int size_px);
void weather_app_register(void);
