// The crash hook's ring of app names (#520, panic_ring.c): a few entries for the apps that ran lately, never one per
// registered app; the oldest is replaced, an app that left is dropped.
#include "panic_ring.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static uint32_t ctx_of(uint32_t slot) {
    return (0x1000u + slot) << 8 | slot;
}

int main(void) {
    static panic_ring_t r;
    CHECK(panic_ring_find(&r, ctx_of(1)) == NULL && panic_ring_find(&r, 0) == NULL); // empty, and ctx 0 is never named
    panic_ring_note(&r, ctx_of(1), "meshcore", false);
    panic_ring_note(&r, ctx_of(2), "weather", false);
    CHECK(strcmp(panic_ring_find(&r, ctx_of(1)), "meshcore") == 0 && strcmp(panic_ring_find(&r, ctx_of(2)), "weather") == 0);
    CHECK(panic_ring_find(&r, ctx_of(1) ^ 0x100u) == NULL); // the slot's other generation (a stale ctx) is not named
    panic_ring_note(&r, ctx_of(1), "other", false);                  // already there: unchanged
    CHECK(strcmp(panic_ring_find(&r, ctx_of(1)), "meshcore") == 0 && r.next == 2);
    panic_ring_note(&r, 0, "x", false);
    panic_ring_note(&r, ctx_of(3), NULL, false);
    CHECK(r.next == 2 && !panic_ring_has(&r, ctx_of(3)));
    // Many more apps than entries: the newest eight are named, the oldest are not (the caller says "#slot")
    for (uint32_t s = 3; s <= 100; s++) {
        char id[16];
        snprintf(id, sizeof(id), "app%u", (unsigned)s);
        panic_ring_note(&r, ctx_of(s), id, false);
    }
    for (uint32_t s = 93; s <= 100; s++) {
        char id[16];
        snprintf(id, sizeof(id), "app%u", (unsigned)s);
        CHECK(panic_ring_find(&r, ctx_of(s)) && strcmp(panic_ring_find(&r, ctx_of(s)), id) == 0);
    }
    CHECK(panic_ring_find(&r, ctx_of(1)) == NULL && panic_ring_find(&r, ctx_of(92)) == NULL);
    // A long id is cut to the cap
    panic_ring_note(&r, ctx_of(200), "an_overlong_app_id", false);
    CHECK(strlen(panic_ring_find(&r, ctx_of(200))) == PANIC_RING_ID_CAP - 1);
    // An app that left is forgotten, and its entry is free to be named again
    panic_ring_drop(&r, ctx_of(100));
    CHECK(panic_ring_find(&r, ctx_of(100)) == NULL && !panic_ring_has(&r, ctx_of(100)));
    panic_ring_drop(&r, ctx_of(7)); // not there: nothing happens
    // An app that runs on its own (it started a task) stays named through any number of hooks entering other contexts
    static panic_ring_t p;
    panic_ring_note(&p, ctx_of(1), "meshcore", true);
    for (uint32_t s = 2; s <= 40; s++) { // 39 distinct apps enter: the pinned one is never the one replaced
        char id[16];
        snprintf(id, sizeof(id), "app%u", (unsigned)s);
        panic_ring_note(&p, ctx_of(s), id, false);
        CHECK(strcmp(panic_ring_find(&p, ctx_of(1)), "meshcore") == 0);
    }
    CHECK(panic_ring_find(&p, ctx_of(40)) && panic_ring_find(&p, ctx_of(34)) && !panic_ring_has(&p, ctx_of(33)));
    // A context that entered first and started a task later becomes pinned where it is
    panic_ring_note(&p, ctx_of(36), "app36", true);
    for (uint32_t s = 50; s < 80; s++) {
        panic_ring_note(&p, ctx_of(s), "other", false);
    }
    CHECK(panic_ring_has(&p, ctx_of(36)) && panic_ring_has(&p, ctx_of(1)));
    // When every entry is pinned the oldest goes; a dropped app frees its pin
    static panic_ring_t q;
    for (uint32_t s = 1; s <= PANIC_RING_SIZE; s++) {
        panic_ring_note(&q, ctx_of(s), "pinned", true);
    }
    panic_ring_note(&q, ctx_of(90), "newer", true);
    CHECK(panic_ring_has(&q, ctx_of(90)) && !panic_ring_has(&q, ctx_of(1)) && panic_ring_has(&q, ctx_of(2)));
    panic_ring_drop(&q, ctx_of(5));
    panic_ring_note(&q, ctx_of(91), "plain", false);
    panic_ring_note(&q, ctx_of(92), "plain2", false); // replaces the unpinned one, not a pinned app
    CHECK(!panic_ring_has(&q, ctx_of(91)) && panic_ring_has(&q, ctx_of(92)) && panic_ring_has(&q, ctx_of(2)));
    if (failures) {
        printf("panic_ring_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("panic_ring_test: all passed\n");
    return 0;
}
