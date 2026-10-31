// Host tests for the screenshot's compositing (#30): ARGB8888 laid over RGB565
#include "shot_blend.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define W 4
#define H 3

static void fill(uint16_t *dst, uint16_t v) {
    for (int i = 0; i < W * H; i++) {
        dst[i] = v;
    }
}

static void test_alpha(void) {
    uint16_t dst[W * H];
    // B, G, R, A: pure red at three alphas
    uint8_t clear[4] = {0, 0, 255, 0}, half[4] = {0, 0, 255, 128}, solid[4] = {0, 0, 255, 255};

    fill(dst, 0x001F); // blue
    shot_blend_argb8888((uint8_t *)dst, W, H, W * 2, clear, 1, 1, 4, 1, 1);
    CHECK(dst[1 * W + 1] == 0x001F); // alpha 0 leaves the screen alone

    shot_blend_argb8888((uint8_t *)dst, W, H, W * 2, solid, 1, 1, 4, 1, 1);
    CHECK(dst[1 * W + 1] == 0xF800); // alpha 255 replaces it
    CHECK(dst[0] == 0x001F && dst[2 * W + 3] == 0x001F);

    fill(dst, 0x001F);
    shot_blend_argb8888((uint8_t *)dst, W, H, W * 2, half, 1, 1, 4, 2, 0);
    uint16_t p = dst[2];
    CHECK(((p >> 11) & 0x1F) >= 15 && ((p >> 11) & 0x1F) <= 16); // about half red ...
    CHECK((p & 0x1F) >= 15 && (p & 0x1F) <= 16);                   // ... and half blue
    CHECK(((p >> 5) & 0x3F) == 0);
}

static void test_clip(void) {
    uint16_t dst[W * H];
    // a 3x2 solid white image
    uint8_t img[3 * 2 * 4];
    memset(img, 255, sizeof(img));

    fill(dst, 0);
    shot_blend_argb8888((uint8_t *)dst, W, H, W * 2, img, 3, 2, 3 * 4, -1, -1); // top-left corner hangs out
    CHECK(dst[0] == 0xFFFF && dst[1] == 0xFFFF && dst[2] == 0);
    CHECK(dst[W] == 0);

    fill(dst, 0);
    shot_blend_argb8888((uint8_t *)dst, W, H, W * 2, img, 3, 2, 3 * 4, 3, 2); // bottom-right corner hangs out
    CHECK(dst[2 * W + 3] == 0xFFFF);
    int lit = 0;
    for (int i = 0; i < W * H; i++) {
        lit += dst[i] != 0;
    }
    CHECK(lit == 1);

    fill(dst, 0);
    shot_blend_argb8888((uint8_t *)dst, W, H, W * 2, img, 3, 2, 3 * 4, 9, 9); // wholly outside
    shot_blend_argb8888((uint8_t *)dst, W, H, W * 2, img, 3, 2, 3 * 4, -9, -9);
    for (int i = 0; i < W * H; i++) {
        CHECK(dst[i] == 0);
    }
}

int main(void) {
    test_alpha();
    test_clip();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("shot_blend_test ok\n");
    return 0;
}
