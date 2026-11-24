// Host test: the set of an SD app's live blocks (#536), which the loader drains when the app unloads: add, remove, growth,
// removed markers, a table that cannot be made, and a drain that visits every pointer once.
#include "ptr_set.h"

#include <stdio.h>
#include <stdlib.h>

static int failures, tables, fail_alloc;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void *alloc_zeroed(size_t bytes) {
    if (fail_alloc) {
        return NULL;
    }
    tables++;
    return calloc(1, bytes);
}
static void release(void *t) {
    tables--;
    free(t);
}
static ptr_set_env_t const env = {alloc_zeroed, release};

static void *fake(uintptr_t i) {
    return (void *)(0x3c000000u + i * 16u);
}

static void test_basic(void) {
    ptr_set_t s = {0};
    CHECK(ptr_set_pop(&s) == NULL && !ptr_set_remove(&s, fake(1))); // nothing yet, no table
    CHECK(ptr_set_add(&s, &env, fake(1)) && ptr_set_add(&s, &env, fake(1)) && s.used == 1); // once
    CHECK(!ptr_set_add(&s, &env, NULL) && !ptr_set_add(&s, &env, PTR_SET_GONE));
    CHECK(ptr_set_remove(&s, fake(1)) && !ptr_set_remove(&s, fake(1))); // a double free finds nothing
    CHECK(s.used == 0 && ptr_set_pop(&s) == NULL);
    ptr_set_release(&s, &env);
    CHECK(tables == 0 && s.cap == 0);
}

// Many pointers: the table grows, every one is found and drains once; removed ones do not come back
static void test_grow_and_drain(void) {
    ptr_set_t s = {0};
    for (uintptr_t i = 1; i <= 1000; i++) {
        CHECK(ptr_set_add(&s, &env, fake(i)));
    }
    CHECK(s.used == 1000 && s.cap >= 1334);
    for (uintptr_t i = 1; i <= 1000; i += 2) {
        CHECK(ptr_set_remove(&s, fake(i)));
    }
    CHECK(s.used == 500);
    for (uintptr_t i = 1; i <= 1000; i += 4) { // a removed one, added again, is live again
        CHECK(ptr_set_add(&s, &env, fake(i)));
    }
    CHECK(s.used == 750);
    static unsigned char seen[1001];
    void *p;
    unsigned n = 0;
    while ((p = ptr_set_pop(&s))) {
        uintptr_t i = ((uintptr_t)p - 0x3c000000u) / 16u;
        if (i < 1 || i > 1000) {
            CHECK(0 && "a pointer the set never held");
            continue;
        }
        CHECK(!seen[i]);
        seen[i] = 1;
        n++;
    }
    CHECK(n == 750 && s.used == 0);
    CHECK(seen[1] && !seen[3] && seen[2] && seen[5]);
    ptr_set_release(&s, &env);
    CHECK(tables == 0);
}

// Churn of adds and removes through the markers: the table does not fill with them
static void test_churn(void) {
    ptr_set_t s = {0};
    for (uintptr_t i = 1; i <= 20000; i++) {
        CHECK(ptr_set_add(&s, &env, fake(i)));
        CHECK(ptr_set_remove(&s, fake(i)));
    }
    CHECK(s.used == 0 && s.cap <= 64);
    CHECK(ptr_set_add(&s, &env, fake(5)) && ptr_set_remove(&s, fake(5)));
    ptr_set_release(&s, &env);
}

// No memory for a bigger table: the pointer is not taken and the set is as it was
static void test_no_memory(void) {
    ptr_set_t s = {0};
    fail_alloc = 1;
    CHECK(!ptr_set_add(&s, &env, fake(1)) && s.used == 0);
    fail_alloc = 0;
    for (uintptr_t i = 1; i <= 12; i++) {
        CHECK(ptr_set_add(&s, &env, fake(i))); // 12 of 16 slots: the next one needs a bigger table
    }
    fail_alloc = 1;
    CHECK(!ptr_set_add(&s, &env, fake(13)) && s.used == 12);
    CHECK(ptr_set_remove(&s, fake(3)) && ptr_set_remove(&s, fake(12)));
    fail_alloc = 0;
    unsigned n = 0;
    while (ptr_set_pop(&s)) {
        n++;
    }
    CHECK(n == 10);
    ptr_set_release(&s, &env);
    CHECK(tables == 0);
}

int main(void) {
    test_basic();
    test_grow_and_drain();
    test_churn();
    test_no_memory();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ptr_set_test ok\n");
    return 0;
}
