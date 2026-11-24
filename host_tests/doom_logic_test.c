// Host test: the DOOM app's key map, key ring, fps windows, WAD percent and 2x picture (#526)
#include "doom_logic.h"

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

static void keys_map(void) {
    CHECK(doom_key_for_code(0x37) == DOOM_KEY_UPARROW);
    CHECK(doom_key_for_code(0x17) == DOOM_KEY_UPARROW);    // W
    CHECK(doom_key_for_code(0x21) == DOOM_KEY_DOWNARROW);  // S
    CHECK(doom_key_for_code(0x20) == DOOM_KEY_STRAFE_L);   // A
    CHECK(doom_key_for_code(0x22) == DOOM_KEY_STRAFE_R);   // D
    CHECK(doom_key_for_code(0x33) == DOOM_KEY_LEFTARROW);
    CHECK(doom_key_for_code(0x35) == DOOM_KEY_RIGHTARROW);
    CHECK(doom_key_for_code(0x3d) == DOOM_KEY_FIRE);
    CHECK(doom_key_for_code(0x41) == DOOM_KEY_USE);
    CHECK(doom_key_for_code(0x43) == DOOM_KEY_USE);
    CHECK(doom_key_for_code(0x29) == DOOM_KEY_RSHIFT);
    CHECK(doom_key_for_code(0x01) == DOOM_KEY_ESCAPE);
    CHECK(doom_key_for_code(0x3b) == DOOM_KEY_ENTER);
    CHECK(doom_key_for_code(0x0c) == '1');
    CHECK(doom_key_for_code(0x12) == '7');
    CHECK(doom_key_for_code(0x1b) == 'y');
    // the fps toggle and the keys the game has no use for are not game keys
    CHECK(doom_key_for_code(DOOM_CODE_FPS_TOGGLE) == DOOM_KEY_NONE);
    CHECK(doom_key_for_code(0x1f) == DOOM_KEY_NONE); // Fn
    CHECK(doom_key_for_code(0x02) == DOOM_KEY_NONE); // a shape key
    CHECK(doom_key_for_code(0) == DOOM_KEY_NONE);
    CHECK(doom_key_for_code(0xff) == DOOM_KEY_NONE);
}

static void keys_queue(void) {
    doom_keyq_t q;
    doom_keyq_init(&q);
    bool    p;
    uint8_t k;
    CHECK(!doom_keyq_pop(&q, &p, &k));
    CHECK(doom_keyq_push(&q, true, DOOM_KEY_FIRE));
    CHECK(doom_keyq_push(&q, false, DOOM_KEY_FIRE));
    CHECK(doom_keyq_pop(&q, &p, &k) && p && k == DOOM_KEY_FIRE);
    CHECK(doom_keyq_pop(&q, &p, &k) && !p && k == DOOM_KEY_FIRE);
    CHECK(!doom_keyq_pop(&q, &p, &k));
    // full: the 33rd event is dropped, the 32 before it survive in order, and it wraps
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < DOOM_KEYQ_SIZE; i++) {
            CHECK(doom_keyq_push(&q, true, (uint8_t)(i + 1)));
        }
        CHECK(!doom_keyq_push(&q, true, 0x55));
        for (int i = 0; i < DOOM_KEYQ_SIZE; i++) {
            CHECK(doom_keyq_pop(&q, &p, &k) && p && k == i + 1);
        }
        CHECK(!doom_keyq_pop(&q, &p, &k));
    }
}

static void fps_windows(void) {
    doom_fps_t f;
    doom_fps_reset(&f);
    CHECK(doom_fps_avg_x10(&f) == 0 && doom_fps_min(&f) == 0);
    // 3 seconds at 30 fps (a frame every 33 ms, one in three 34), then the window of the next second is still open
    uint32_t t = 5000;
    for (int i = 0; i < 90; i++) {
        doom_fps_frame(&f, t);
        t += (i % 3 == 2) ? 34 : 33;
    }
    doom_fps_advance(&f, t);
    CHECK(f.windows == 2 || f.windows == 3);
    CHECK(doom_fps_avg_x10(&f) >= 295 && doom_fps_avg_x10(&f) <= 305);
    CHECK(doom_fps_min(&f) >= 29 && doom_fps_min(&f) <= 31);
}

static void fps_stall_and_min(void) {
    doom_fps_t f;
    doom_fps_reset(&f);
    for (uint32_t t = 0; t < 1000; t += 25) { // 40 fps
        doom_fps_frame(&f, t);
    }
    for (uint32_t t = 1000; t < 2000; t += 50) { // 20 fps: the lowest window
        doom_fps_frame(&f, t);
    }
    doom_fps_frame(&f, 4500); // a 2.5 s stall: two windows of 0 frames
    CHECK(f.windows == 4);
    CHECK(doom_fps_min(&f) == 0);
    CHECK(doom_fps_avg_x10(&f) == (40 + 20 + 0 + 0) * 10 / 4);
    // the wrap of the millisecond tick does not make a window of 49 days
    doom_fps_reset(&f);
    doom_fps_frame(&f, 0xfffffff0u);
    doom_fps_frame(&f, 0xfffffff0u + 1500u);
    CHECK(f.windows == 1);
}

static void load_percent(void) {
    CHECK(doom_load_percent(0, 0) == 0);
    CHECK(doom_load_percent(0, 1000) == 0);
    CHECK(doom_load_percent(500, 1000) == 50);
    CHECK(doom_load_percent(1000, 1000) == 99); // 100 is the first frame
    CHECK(doom_load_percent(5000, 1000) == 99); // lumps read twice
    CHECK(doom_load_percent(0xffffffffu, 0xffffffffu) == 99);
}

static void picture(void) {
    CHECK(doom_rgb565(255, 255, 255) == 0xFFFF);
    CHECK(doom_rgb565(255, 0, 0) == 0xF800);
    CHECK(doom_rgb565(0, 255, 0) == 0x07E0);
    CHECK(doom_rgb565(0, 0, 255) == 0x001F);
    uint16_t pal[256];
    for (int i = 0; i < 256; i++) {
        pal[i] = (uint16_t)(0x1000 + i);
    }
    static uint8_t  src[DOOM_SRC_W * DOOM_SRC_H];
    static uint16_t dst[DOOM_SRC_W * 2 * DOOM_SRC_H * 2];
    for (int i = 0; i < DOOM_SRC_W * DOOM_SRC_H; i++) {
        src[i] = (uint8_t)(i * 7);
    }
    memset(dst, 0xee, sizeof(dst));
    doom_expand_2x(dst, src, pal);
    int bad = 0;
    for (int y = 0; y < DOOM_SRC_H * 2; y++) {
        for (int x = 0; x < DOOM_SRC_W * 2; x++) {
            if (dst[y * DOOM_SRC_W * 2 + x] != pal[src[(y / 2) * DOOM_SRC_W + x / 2]]) {
                bad++;
            }
        }
    }
    CHECK(bad == 0);
}

static void picture_i8(void) {
    static uint8_t src[DOOM_SRC_W * DOOM_SRC_H];
    static uint8_t dst[DOOM_SRC_W * 2 * DOOM_SRC_H * 2 + 8];
    for (int i = 0; i < DOOM_SRC_W * DOOM_SRC_H; i++) {
        src[i] = (uint8_t)(i * 13 + 5);
    }
    memset(dst, 0xee, sizeof(dst));
    doom_expand_2x_i8(dst, src);
    int bad = 0;
    for (int y = 0; y < DOOM_SRC_H * 2; y++) {
        for (int x = 0; x < DOOM_SRC_W * 2; x++) {
            if (dst[y * DOOM_SRC_W * 2 + x] != src[(y / 2) * DOOM_SRC_W + x / 2]) {
                bad++;
            }
        }
    }
    CHECK(bad == 0);
    CHECK(dst[DOOM_SRC_W * 2 * DOOM_SRC_H * 2] == 0xee); // nothing written past the picture
}

int main(void) {
    keys_map();
    keys_queue();
    fps_windows();
    fps_stall_and_min();
    load_percent();
    picture();
    picture_i8();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("doom_logic_test: ok\n");
    return 0;
}
