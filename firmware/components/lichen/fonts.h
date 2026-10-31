#pragma once

// Lichen's UI fonts (#16): bitmap fonts from the assets partition with accents, Cyrillic, Greek and emoji, or LVGL's
// built-in Montserrat (ASCII only) when the partition has no usable font.

#include "lvgl.h"

#define LICHEN_FONT_SMALL  24 // body text, status bar
#define LICHEN_FONT_MEDIUM 28 // MeshCore's list rows
#define LICHEN_FONT_LARGE  40 // headings

// Load the fonts from the assets partition into PSRAM and register "fw fonts". After LVGL and assets_mount().
void lichen_fonts_load(void);
// The font for a size above. Never NULL: before lichen_fonts_load(), or when a file is missing or damaged, it is
// the built-in Montserrat.
lv_font_t const *lichen_font(int px);
