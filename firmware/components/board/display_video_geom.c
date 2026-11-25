#include "display_video_geom.h"

uint32_t display_video_scale16(uint32_t milli) {
    uint64_t const v = ((uint64_t)milli * DISPLAY_VIDEO_SCALE_STEPS + 500u) / 1000u;
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

static bool rect_empty(display_rect_t r) {
    return r.x2 < r.x1 || r.y2 < r.y1;
}

bool display_video_geom_make(display_video_geom_t *g, uint32_t src_w, uint32_t src_h, int32_t ui_w, int32_t ui_h,
                             uint32_t want_x_milli, uint32_t want_y_milli) {
    uint32_t const sx = display_video_scale16(want_x_milli), sy = display_video_scale16(want_y_milli);
    if (src_w == 0 || src_h == 0 || sx == 0 || sy == 0 || ui_w <= 0 || ui_h <= 0 || src_w > 0xFFFFu ||
        src_h > 0xFFFFu || sx > 0xFFFFu || sy > 0xFFFFu)
        return false;
    uint64_t const w = (uint64_t)src_w * sx / DISPLAY_VIDEO_SCALE_STEPS;
    uint64_t const h = (uint64_t)src_h * sy / DISPLAY_VIDEO_SCALE_STEPS;
    if (w == 0 || h == 0 || w > (uint64_t)ui_w || h > (uint64_t)ui_h)
        return false;
    int32_t const x1 = (ui_w - (int32_t)w) / 2, y1 = (ui_h - (int32_t)h) / 2;
    g->src_w = src_w; g->src_h = src_h; g->scale_x16 = sx; g->scale_y16 = sy; g->ui_hres = ui_w;
    g->rect = (display_rect_t){x1, y1, x1 + (int32_t)w - 1, y1 + (int32_t)h - 1};
    g->rect_blk = display_rot90_block(g->rect, ui_w);
    g->bar[0] = (display_rect_t){0, 0, ui_w - 1, g->rect.y1 - 1};
    g->bar[1] = (display_rect_t){0, g->rect.y2 + 1, ui_w - 1, ui_h - 1};
    for (int i = 0; i < 2; i++) {
        g->bar_ok[i] = !rect_empty(g->bar[i]);
        g->bar_blk[i] = g->bar_ok[i] ? display_rot90_block(g->bar[i], ui_w) : (display_rect_t){0, 0, -1, -1};
    }
    return true;
}

bool display_video_hits(display_video_geom_t const *g, display_rect_t s) {
    if (rect_empty(s))
        return false;
    return s.x1 <= g->rect.x2 && s.x2 >= g->rect.x1 && s.y1 <= g->rect.y2 && s.y2 >= g->rect.y1;
}

uint32_t display_video_split(display_video_geom_t const *g, display_rect_t s,
                             display_rect_t out[DISPLAY_VIDEO_SPLIT_MAX]) {
    if (rect_empty(s))
        return 0;
    if (!display_video_hits(g, s)) {
        out[0] = s;
        return 1;
    }
    uint32_t       n = 0;
    display_rect_t const v = g->rect;
    // Full-width bands above and below, then the sides only over the rows the video covers
    if (s.y1 < v.y1)
        out[n++] = (display_rect_t){s.x1, s.y1, s.x2, v.y1 - 1};
    if (s.y2 > v.y2)
        out[n++] = (display_rect_t){s.x1, v.y2 + 1, s.x2, s.y2};
    int32_t const y1 = s.y1 > v.y1 ? s.y1 : v.y1, y2 = s.y2 < v.y2 ? s.y2 : v.y2;
    if (s.x1 < v.x1)
        out[n++] = (display_rect_t){s.x1, y1, v.x1 - 1, y2};
    if (s.x2 > v.x2)
        out[n++] = (display_rect_t){v.x2 + 1, y1, s.x2, y2};
    return n;
}
