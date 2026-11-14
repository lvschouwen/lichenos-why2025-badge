// Screenshot compositing (#30): the status bar and the update banner live on LVGL's top layer, which a snapshot of the
// active screen does not include. Each of them is snapshotted as ARGB8888 and laid over the RGB565 screen here.
// Pure and host-tested.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Lay an ARGB8888 image (LVGL byte order B, G, R, A; straight alpha) over an RGB565 image with its top-left corner at
// (x, y), which may be negative; the parts outside dst are clipped. Strides are in bytes.
typedef struct {
    uint8_t *px;
    int      w;
    int      h;
    size_t   stride;
} shot_img_t;

void shot_blend_argb8888(shot_img_t const *dst, shot_img_t const *src, int x, int y);
