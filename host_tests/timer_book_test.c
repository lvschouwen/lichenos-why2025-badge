// Host test: lichen_timer's book (#231, timer_book.c): handles are never reused, a stale or made-up one matches
// nothing, a full book refuses, and an app's timers are found and counted by its context (what its stop drops).
#include "timer_book.h"

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

#define WX   0x65432102u
#define MESH 0x12345601u

static int ran;
static void cb(void *arg) {
    (void)arg;
    ran++;
}

static void test_add_find_remove(void) {
    static timer_book_t b;
    memset(&b, 0, sizeof(b));
    int      tag = 7;
    uint32_t h   = timer_book_add(&b, WX, cb, &tag);
    CHECK(h != 0);
    timer_slot_t const *s = timer_book_find(&b, h);
    CHECK(s && s->ctx == WX && s->cb == cb && s->arg == &tag);
    CHECK(timer_book_count(&b, WX) == 1 && timer_book_count(&b, MESH) == 0);
    CHECK(timer_book_remove(&b, h));
    CHECK(!timer_book_remove(&b, h)); // twice: nothing
    CHECK(timer_book_find(&b, h) == NULL && timer_book_count(&b, WX) == 0);
    // The slot is reused with a new generation: the old handle stays stale
    uint32_t h2 = timer_book_add(&b, MESH, cb, NULL);
    CHECK(h2 && h2 != h && timer_book_find(&b, h) == NULL && timer_book_find(&b, h2)->ctx == MESH);
    CHECK(!timer_book_remove(&b, h));
    CHECK(timer_book_find(&b, h2) != NULL);
}

static void test_refused(void) {
    static timer_book_t b;
    memset(&b, 0, sizeof(b));
    CHECK(timer_book_add(&b, 0, cb, NULL) == 0);  // no context
    CHECK(timer_book_add(&b, WX, NULL, NULL) == 0); // no callback
    CHECK(timer_book_find(&b, 0) == NULL && !timer_book_remove(&b, 0));
    // Made-up handles: slot 0 or past the book, a slot's right index with another generation
    CHECK(timer_book_find(&b, 0xffffffe0u) == NULL);
    CHECK(timer_book_find(&b, (1u << TIMER_BOOK_SLOT_BITS) | 31u) == NULL);
    uint32_t h = timer_book_add(&b, WX, cb, NULL);
    CHECK(timer_book_find(&b, h ^ (1u << TIMER_BOOK_SLOT_BITS)) == NULL);
    CHECK(!timer_book_remove(&b, h ^ (1u << TIMER_BOOK_SLOT_BITS)) && timer_book_find(&b, h) != NULL);
}

static void test_full_and_by_ctx(void) {
    static timer_book_t b;
    memset(&b, 0, sizeof(b));
    uint32_t h[TIMER_BOOK_SLOTS];
    for (int i = 0; i < TIMER_BOOK_SLOTS; i++) {
        h[i] = timer_book_add(&b, (i % 2) ? WX : MESH, cb, NULL);
        CHECK(h[i] != 0);
    }
    CHECK(timer_book_add(&b, WX, cb, NULL) == 0); // full
    CHECK(timer_book_count(&b, WX) == TIMER_BOOK_SLOTS / 2 && timer_book_count(&b, MESH) == TIMER_BOOK_SLOTS / 2);
    CHECK(timer_book_count(&b, 0) == 0);
    // An app's stop: every one of its timers, removed while walking, the other app's untouched
    size_t   from = 0, dropped = 0;
    uint32_t got;
    while ((got = timer_book_next(&b, WX, &from)) != 0) {
        CHECK(timer_book_find(&b, got)->ctx == WX);
        CHECK(timer_book_remove(&b, got));
        dropped++;
    }
    CHECK(dropped == TIMER_BOOK_SLOTS / 2 && timer_book_count(&b, WX) == 0);
    CHECK(timer_book_count(&b, MESH) == TIMER_BOOK_SLOTS / 2);
    from = 0;
    CHECK(timer_book_next(&b, WX, &from) == 0 && from == TIMER_BOOK_SLOTS);
    from = 0;
    CHECK(timer_book_next(&b, 0, &from) == 0);
    CHECK(timer_book_add(&b, WX, cb, NULL) != 0); // room again
}

// The generation wraps without ever making handle 0 or a handle still live
static void test_generation_wrap(void) {
    static timer_book_t b;
    memset(&b, 0, sizeof(b));
    b.gen      = (1u << (32u - TIMER_BOOK_SLOT_BITS)) - 2u;
    uint32_t a = timer_book_add(&b, WX, cb, NULL);
    uint32_t c = timer_book_add(&b, WX, cb, NULL);
    uint32_t d = timer_book_add(&b, WX, cb, NULL); // the generation came round to 1
    CHECK(a && c && d && a != c && c != d && a != d);
    CHECK(timer_book_find(&b, a) && timer_book_find(&b, c) && timer_book_find(&b, d));
}

int main(void) {
    test_add_find_remove();
    test_refused();
    test_full_and_by_ctx();
    test_generation_wrap();
    (void)ran;
    if (failures) {
        printf("timer_book_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("timer_book_test: all passed\n");
    return 0;
}
