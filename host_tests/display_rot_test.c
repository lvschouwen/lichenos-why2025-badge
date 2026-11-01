// Host tests for the UI-area -> panel-block mapping of the direct flush (#95). The reference is
// esp_lvgl_port 2.9's own maths, copied here verbatim in shape: lvgl_port_rotate_area (software path) and
// lvgl_port_ppa_rotate (PPA path), both for LV_DISPLAY_ROTATION_90. A pixel-level check rotates a strip
// the way LVGL's rotate90_rgb565 does and checks every pixel lands at panel (y, H - 1 - x).
#include "display_rot.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// lvgl_port_rotate_area, case LV_DISPLAY_ROTATION_90 (vres there = the UI horizontal resolution)
static display_rect_t port_sw_rotate_area(display_rect_t a, int32_t ui_hres) {
    int32_t const w = a.x2 - a.x1 + 1;
    int32_t const h = a.y2 - a.y1 + 1;
    int32_t const vres = ui_hres;
    a.y2 = vres - a.x1 - 1;
    a.x1 = a.y1;
    a.x2 = a.x1 + h - 1;
    a.y1 = a.y2 - w + 1;
    return a;
}

// lvgl_port_ppa_rotate, case PPA_SRM_ROTATION_ANGLE_90 (disp_size.hres = lv_display_get_horizontal_resolution)
static display_rect_t port_ppa_rotate_area(display_rect_t a, int32_t ui_hres) {
    display_rect_t const r = {
        .x1 = a.y1,
        .x2 = a.y2,
        .y1 = ui_hres - a.x2 - 1,
        .y2 = ui_hres - a.x1 - 1,
    };
    return r;
}

static int same(display_rect_t a, display_rect_t b) {
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

static void check_against_port(display_rect_t ui, int32_t ui_hres) {
    display_rect_t const got = display_rot90_block(ui, ui_hres);
    CHECK(same(got, port_sw_rotate_area(ui, ui_hres)));
    CHECK(same(got, port_ppa_rotate_area(ui, ui_hres)));
    CHECK(got.x2 - got.x1 == ui.y2 - ui.y1); // a w x h strip becomes h x w
    CHECK(got.y2 - got.y1 == ui.x2 - ui.x1);
}

// LVGL rotate90_rgb565, generic C path: dst[(w - 1 - x) * dst_stride + y] = src[y * src_stride + x]
static void lvgl_rotate90(uint16_t const *src, uint16_t *dst, int32_t w, int32_t h, int32_t src_stride,
                          int32_t dst_stride) {
    for (int32_t x = 0; x < w; ++x) {
        for (int32_t y = 0; y < h; ++y) {
            dst[(w - x - 1) * dst_stride + y] = src[y * src_stride + x];
        }
    }
}

// Render a small UI area, rotate it into a panel frame buffer at the computed block, check every pixel
static void check_pixels(display_rect_t ui, int32_t n) {
    enum { MAX = 16 };
    uint16_t       strip[MAX * MAX];
    uint16_t       fb[MAX * MAX];
    int32_t const  w = ui.x2 - ui.x1 + 1;
    int32_t const  h = ui.y2 - ui.y1 + 1;
    display_rect_t blk = display_rot90_block(ui, n);
    memset(fb, 0, sizeof(fb));
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            strip[y * w + x] = (uint16_t)(((ui.y1 + y) << 8) | (ui.x1 + x) | 0x8000); // encodes its UI position
        }
    }
    lvgl_rotate90(strip, fb + blk.y1 * n + blk.x1, w, h, w, n);
    for (int32_t py = 0; py < n; py++) {
        for (int32_t px = 0; px < n; px++) {
            int32_t const ux = n - 1 - py, uy = px; // panel (px, py) shows UI (n - 1 - py, px)
            int const     inside = ux >= ui.x1 && ux <= ui.x2 && uy >= ui.y1 && uy <= ui.y2;
            uint16_t const want  = inside ? (uint16_t)((uy << 8) | ux | 0x8000) : 0;
            CHECK(fb[py * n + px] == want);
        }
    }
}

int main(void) {
    int32_t const N = 720;
    // the four corner strips of the badge's 720x720 UI, 80-line strips (draw buffer is 720x80)
    check_against_port((display_rect_t){0, 0, N - 1, 79}, N);            // top strip
    check_against_port((display_rect_t){0, N - 80, N - 1, N - 1}, N);    // bottom strip
    check_against_port((display_rect_t){0, 0, 0, 0}, N);                 // top-left pixel
    check_against_port((display_rect_t){N - 1, 0, N - 1, 0}, N);         // top-right pixel
    check_against_port((display_rect_t){0, N - 1, 0, N - 1}, N);         // bottom-left pixel
    check_against_port((display_rect_t){N - 1, N - 1, N - 1, N - 1}, N); // bottom-right pixel
    check_against_port((display_rect_t){0, 320, N - 1, 399}, N);         // middle strip
    check_against_port((display_rect_t){600, 0, 719, 31}, N);            // a status-bar-sized partial area

    // explicit values, so a change in both the port and this code can't pass silently
    display_rect_t const top = display_rot90_block((display_rect_t){0, 0, N - 1, 79}, N);
    CHECK(top.x1 == 0 && top.x2 == 79 && top.y1 == 0 && top.y2 == N - 1); // UI top = panel left columns
    display_rect_t const mid = display_rot90_block((display_rect_t){0, 320, N - 1, 399}, N);
    CHECK(mid.x1 == 320 && mid.x2 == 399 && mid.y1 == 0 && mid.y2 == N - 1);
    display_rect_t const tr = display_rot90_block((display_rect_t){N - 1, 0, N - 1, 0}, N);
    CHECK(tr.x1 == 0 && tr.y1 == 0); // UI top-right = panel top-left
    display_rect_t const tl = display_rot90_block((display_rect_t){0, 0, 0, 0}, N);
    CHECK(tl.x1 == 0 && tl.y1 == N - 1); // UI top-left = panel bottom-left
    display_rect_t const br = display_rot90_block((display_rect_t){N - 1, N - 1, N - 1, N - 1}, N);
    CHECK(br.x1 == N - 1 && br.y1 == 0); // UI bottom-right = panel top-right

    // pixel level on a 16x16 screen: full-width strips, a middle block and the corners
    check_pixels((display_rect_t){0, 0, 15, 3}, 16);
    check_pixels((display_rect_t){0, 12, 15, 15}, 16);
    check_pixels((display_rect_t){5, 6, 10, 8}, 16);
    check_pixels((display_rect_t){0, 0, 0, 0}, 16);
    check_pixels((display_rect_t){15, 15, 15, 15}, 16);

    if (failures) {
        printf("display_rot_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("display_rot_test: ok\n");
    return 0;
}
