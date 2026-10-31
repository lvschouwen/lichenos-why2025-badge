#include "shot_blend.h"

static uint32_t mix(uint32_t over, uint32_t under, uint32_t alpha) {
    return (over * alpha + under * (255 - alpha) + 127) / 255;
}

static void blend_pixel(uint16_t *d, uint8_t const *s) {
    uint32_t a = s[3];
    if (a == 0) {
        return;
    }
    uint32_t r, g, b;
    if (a == 255) {
        r = s[2];
        g = s[1];
        b = s[0];
    } else {
        uint32_t p  = *d;
        uint32_t pr = ((p >> 11) & 0x1F), pg = (p >> 5) & 0x3F, pb = p & 0x1F;
        r           = mix(s[2], (pr << 3) | (pr >> 2), a);
        g           = mix(s[1], (pg << 2) | (pg >> 4), a);
        b           = mix(s[0], (pb << 3) | (pb >> 2), a);
    }
    *d = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void shot_blend_argb8888(uint8_t *dst, int dst_w, int dst_h, size_t dst_stride, uint8_t const *src, int src_w,
                         int src_h, size_t src_stride, int x, int y) {
    for (int row = 0; row < src_h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= dst_h) {
            continue;
        }
        uint16_t *d = (uint16_t *)(void *)(dst + (size_t)dy * dst_stride);
        for (int col = 0; col < src_w; col++) {
            int dx = x + col;
            if (dx >= 0 && dx < dst_w) {
                blend_pixel(&d[dx], src + (size_t)row * src_stride + (size_t)col * 4);
            }
        }
    }
}
