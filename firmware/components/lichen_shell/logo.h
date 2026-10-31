#pragma once

#include "lvgl.h"

// The Lichen logo, "symbiosis" (lichen.html option C): amber nodes (the P4) and teal nodes (the C6) woven around a
// centre node. Drawn with LVGL primitives at any size (16, 32 and about 100 px in use); no image file.
lv_obj_t *logo_create(lv_obj_t *parent, int size_px);
