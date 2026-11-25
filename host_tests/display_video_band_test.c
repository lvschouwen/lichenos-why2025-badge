// Host tests for the held band strips of the OS video surface (#561): placement, replace on the same rectangle, the
// overflow rule (never blocks, remembers where), and that a queue is emptied only by a pop or a clear.
#include "display_video_band.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define CAP (720 * 80 * 2)

static display_rect_t R(int x1, int y1, int x2, int y2) {
    return (display_rect_t){x1, y1, x2, y2};
}

static void test_place(void) {
    display_band_q_t q;
    display_band_q_init(&q, CAP);
    display_band_t const *a = display_band_q_put(&q, R(0, 0, 99, 9)); // 100 x 10 x 2 = 2000 -> 2048
    display_band_t const *b = display_band_q_put(&q, R(0, 10, 9, 19)); // 10 x 10 x 2 = 200 -> 256
    CHECK(a && b && a->off == 0 && b->off == 2048 && q.n == 2 && q.used == 2048 + 256);
    CHECK(display_band_bytes(R(0, 0, 99, 9)) == 2048);
    CHECK(display_band_bytes(R(5, 5, 5, 5)) == 64);
    CHECK(b->off % DISPLAY_BAND_ALIGN == 0);
}

static void test_same_rect_replaces_and_moves_last(void) {
    display_band_q_t q;
    display_band_q_init(&q, CAP);
    display_band_q_put(&q, R(0, 0, 99, 9));
    display_band_q_put(&q, R(50, 0, 149, 9)); // overlaps the first
    uint32_t used = q.used;
    display_band_t const *again = display_band_q_put(&q, R(0, 0, 99, 9));
    CHECK(again && q.n == 2 && q.used == used);        // no new room
    CHECK(again == &q.e[1] && again->off == 0);        // its old place, newest in order: it wins the overlap
    CHECK(q.e[0].ui.x1 == 50);
    CHECK(!q.lost);
}

static void test_same_rect_never_overflows(void) {
    display_band_q_t q;
    display_band_q_init(&q, 2048);
    for (int i = 0; i < 100; i++) { // a paused app: the clock rectangle again and again
        CHECK(display_band_q_put(&q, R(0, 0, 99, 9)) != NULL);
    }
    CHECK(q.n == 1 && !q.lost && q.overflows == 0);
}

static void test_overflow_by_bytes(void) {
    display_band_q_t q;
    display_band_q_init(&q, CAP);
    CHECK(display_band_q_put(&q, R(0, 0, 719, 79)) != NULL); // a whole strip: exactly the buffer
    CHECK(q.used == CAP);
    CHECK(display_band_q_put(&q, R(0, 80, 719, 90)) == NULL);
    CHECK(q.lost && q.overflows == 1 && q.n == 1);
    display_rect_t r;
    CHECK(display_band_q_take_lost(&q, &r) && r.y1 == 80 && r.y2 == 90 && r.x2 == 719);
    CHECK(!display_band_q_take_lost(&q, &r)); // taken once
}

static void test_overflow_by_count_and_union(void) {
    display_band_q_t q;
    display_band_q_init(&q, CAP);
    for (int i = 0; i < DISPLAY_BAND_MAX; i++) {
        CHECK(display_band_q_put(&q, R(i * 4, 0, i * 4 + 3, 3)) != NULL);
    }
    CHECK(display_band_q_put(&q, R(300, 10, 310, 20)) == NULL);
    CHECK(display_band_q_put(&q, R(200, 5, 250, 12)) == NULL);
    display_rect_t r;
    CHECK(display_band_q_take_lost(&q, &r));
    CHECK(r.x1 == 200 && r.y1 == 5 && r.x2 == 310 && r.y2 == 20);
}

static void test_pop_and_clear(void) {
    display_band_q_t q;
    display_band_q_init(&q, 2048);
    display_band_q_put(&q, R(0, 0, 99, 9));
    display_band_q_put(&q, R(0, 20, 99, 29)); // does not fit: lost
    display_band_q_pop_all(&q);
    CHECK(display_band_q_count(&q) == 0 && q.used == 0 && q.lost); // the lost mark survives a present
    CHECK(display_band_q_put(&q, R(0, 20, 99, 29)) != NULL);       // room again
    display_band_q_clear(&q);
    CHECK(display_band_q_count(&q) == 0 && !q.lost);
}

int main(void) {
    test_place();
    test_same_rect_replaces_and_moves_last();
    test_same_rect_never_overflows();
    test_overflow_by_bytes();
    test_overflow_by_count_and_union();
    test_pop_and_clear();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("display_video_band_test: ok\n");
    return 0;
}
