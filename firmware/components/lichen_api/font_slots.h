#pragma once

// The font slots' setter (#220): lichen_os's lichen_fonts_load() hands each font it loaded from the assets partition
// to the gate's slot table (fonts.c), which lichen_font() reads. For lichen_os at boot only; apps never call it.

#include "lvgl.h"

#include <stdbool.h>

// Use font for the size px (one of LICHEN_FONT_*); NULL goes back to the built-in one. False for an unknown size.
bool lichen_font_set(int px, lv_font_t const *font);
