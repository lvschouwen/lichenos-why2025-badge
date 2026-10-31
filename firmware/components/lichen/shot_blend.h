// Screenshot compositing (#30): the status bar and the update banner live on LVGL's top layer, which a snapshot of the
// active screen does not include. Each of them is snapshotted as ARGB8888 and laid over the RGB565 screen here.
// Pure and host-tested.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Lay an ARGB8888 image (LVGL byte order B, G, R, A; straight alpha) over an RGB565 image with its top-left corner at
// (x, y), which may be negative; the parts outside dst are clipped. Strides are in bytes.
void shot_blend_argb8888(uint8_t *dst, int dst_w, int dst_h, size_t dst_stride, uint8_t const *src, int src_w,
                         int src_h, size_t src_stride, int x, int y);
