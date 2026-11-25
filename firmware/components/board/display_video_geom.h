#pragma once

#include "display_rot.h"

#include <stdbool.h>
#include <stdint.h>

// Pure geometry of the OS-owned video surface (#559, part of #527): a small RGB565 source scaled by the PPA (steps
// of 1/16), centred on the UI, the rest of the panel left to LVGL as bars. No ESP-IDF, no LVGL
// (host_tests/display_video_geom_test.c). UI coordinates are LVGL's rotated frame; "blk" is the same area in panel
// (frame buffer) coordinates, mapped by display_rot90_block, so the PPA's output block is rect_blk.

#define DISPLAY_VIDEO_SCALE_STEPS 16 // the PPA's scale granularity: 1/16

// A wanted scale in thousandths (2250 = 2.25) as the nearest 1/16 step (36); 2700 gives 43 (2.6875). 0 for 0.
uint32_t display_video_scale16(uint32_t milli);

typedef struct {
    uint32_t       src_w, src_h;
    uint32_t       scale_x16, scale_y16; // PPA scale in 1/16
    int32_t        ui_hres;              // UI width = panel height
    display_rect_t rect;                 // the video, UI coordinates
    display_rect_t rect_blk;             // the video, panel coordinates
    display_rect_t bar[2];               // UI: above and below the video; w == 0 height-wise = empty (see bar_ok)
    display_rect_t bar_blk[2];
    bool           bar_ok[2]; // the bar has at least one row
} display_video_geom_t;

// Geometry for a src_w x src_h source at the wanted scales (thousandths), centred on a ui_w x ui_h UI. The output is
// src * scale16 / 16 rounded down; the odd pixel of the leftover goes to the bottom/right bar. False when a size or
// scale is 0 or the output does not fit the UI. The bars span the full UI width.
bool display_video_geom_make(display_video_geom_t *g, uint32_t src_w, uint32_t src_h, int32_t ui_w, int32_t ui_h,
                             uint32_t want_x_milli, uint32_t want_y_milli);

// Whether a UI-coordinate strip shares at least one pixel with the video
bool display_video_hits(display_video_geom_t const *g, display_rect_t ui_strip);

// The parts of a UI strip outside the video (up to 4: above, below, left, right; they do not overlap). Returns
// how many were written to out[4]; 0 when the strip is entirely inside the video, 1 (the strip itself) when it is
// entirely outside.
#define DISPLAY_VIDEO_SPLIT_MAX 4
uint32_t display_video_split(display_video_geom_t const *g, display_rect_t ui_strip,
                             display_rect_t out[DISPLAY_VIDEO_SPLIT_MAX]);
