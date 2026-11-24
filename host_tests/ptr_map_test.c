// Host test: the SD apps' live blocks (#536), pointer -> owner in one map, which the loader sweeps per owner when an app
// unloads: note, forget by pointer alone, owners kept apart, address reuse, growth, a table that cannot be made, a sweep that
// frees each block once while others' blocks come and go.
#include "ptr_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static ptr_map_env_t const env = {alloc_zeroed, release};

#define X 0x101u
#define Y 0x202u

static void *fake(uintptr_t i) {
    return (void *)(0x3c000000u + i * 16u);
}

// The loader's sweep, ptr_map_sweep, with a free that counts per index
static int swept[4096];
static void nolock(void *u) {
    (void)u;
}
static void count_free(void *u, void *p) {
    (void)u;
    uintptr_t i = ((uintptr_t)p - 0x3c000000u) / 16u;
    if (i < 4096) {
        swept[i]++;
    }
}
static ptr_map_sweep_env_t const senv = {NULL, nolock, nolock, count_free};
static unsigned sweep(ptr_map_t *m, uint32_t owner) {
    return (unsigned)ptr_map_sweep(m, owner, &senv);
}

static void test_basic(void) {
    ptr_map_t m = {0};
    void     *p;
    uint32_t  c = 0;
    CHECK(!ptr_map_forget(&m, fake(1)) && !ptr_map_take(&m, X, &c, &p)); // no table yet
    CHECK(!ptr_map_note(&m, &env, NULL, X) && !ptr_map_note(&m, &env, fake(1), 0));
    CHECK(ptr_map_note(&m, &env, fake(1), X) && ptr_map_note(&m, &env, fake(1), X) && m.used == 1); // once
    CHECK(ptr_map_forget(&m, fake(1)) && !ptr_map_forget(&m, fake(1))); // a double free finds nothing
    CHECK(m.used == 0);
    ptr_map_trim(&m, &env);
    CHECK(tables == 0 && m.cap == 0);
}

// A free runs under whatever context (a screen's delete callback after the card went runs as the OS): forget has no owner
// argument and must take the entry of any owner out, or the sweep frees the block a second time
static void test_forget_foreign(void) {
    ptr_map_t m = {0};
    CHECK(ptr_map_note(&m, &env, fake(1), X) && ptr_map_note(&m, &env, fake(2), Y));
    CHECK(ptr_map_forget(&m, fake(1))); // freed by "someone else"
    CHECK(sweep(&m, X) == 0);           // nothing of X's left to free twice
    CHECK(sweep(&m, Y) == 1 && m.used == 0);
    ptr_map_trim(&m, &env);
    CHECK(tables == 0);
}

static void test_two_owners(void) {
    ptr_map_t m = {0};
    memset(swept, 0, sizeof swept);
    for (uintptr_t i = 1; i <= 300; i++) {
        CHECK(ptr_map_note(&m, &env, fake(i), i % 2 ? X : Y)); // interleaved
    }
    CHECK(sweep(&m, X) == 150 && m.used == 150);
    for (uintptr_t i = 1; i <= 300; i++) {
        CHECK(swept[i] == (i % 2 ? 1 : 0)); // each of X's once, none of Y's
    }
    CHECK(sweep(&m, X) == 0);
    for (uintptr_t i = 2; i <= 300; i += 2) {
        CHECK(ptr_map_forget(&m, fake(i))); // all of Y's still there
    }
    CHECK(m.used == 0);
    ptr_map_trim(&m, &env);
    CHECK(tables == 0);
}

// The heap hands an address out again: freed under X, then Y's (a stale entry would give the block to the wrong sweep)
static void test_reuse(void) {
    ptr_map_t m = {0};
    CHECK(ptr_map_note(&m, &env, fake(7), X) && ptr_map_forget(&m, fake(7)));
    CHECK(ptr_map_note(&m, &env, fake(7), Y));
    CHECK(sweep(&m, X) == 0 && sweep(&m, Y) == 1);
    CHECK(ptr_map_note(&m, &env, fake(8), X) && ptr_map_note(&m, &env, fake(8), Y) && m.used == 1); // a stale entry: new owner
    CHECK(sweep(&m, X) == 0 && sweep(&m, Y) == 1);
    ptr_map_trim(&m, &env);
}

// Churn of notes and forgets: no removed markers, the table does not grow with it
static void test_churn_and_shift(void) {
    ptr_map_t m = {0};
    for (uintptr_t i = 1; i <= 20000; i++) {
        CHECK(ptr_map_note(&m, &env, fake(i), X) && ptr_map_forget(&m, fake(i)));
    }
    CHECK(m.used == 0 && m.cap == 16);
    // forgets in the middle of clusters keep every other entry findable
    for (uintptr_t i = 1; i <= 1000; i++) {
        CHECK(ptr_map_note(&m, &env, fake(i), X));
    }
    for (uintptr_t i = 1; i <= 1000; i += 3) {
        CHECK(ptr_map_forget(&m, fake(i)));
    }
    for (uintptr_t i = 1; i <= 1000; i++) {
        CHECK(ptr_map_forget(&m, fake(i)) == (i % 3 != 1)); // a second forget of a removed one is refused
    }
    CHECK(m.used == 0);
    ptr_map_trim(&m, &env);
    CHECK(tables == 0);
}

// Others' blocks coming and going while a sweep runs (the mutex is let go between two takes): every one of X's still goes once
static ptr_map_t   *churn_map;
static uintptr_t    other;
static void churn_unlock(void *u) {
    (void)u;
    ptr_map_note(churn_map, &env, fake(other), Y); // another app mallocs, and frees an older one
    if (other > 1000) {
        ptr_map_forget(churn_map, fake(other - 1));
    }
    other++;
}
static void test_sweep_with_churn(void) {
    ptr_map_t m = {0};
    memset(swept, 0, sizeof swept);
    for (uintptr_t i = 1; i <= 500; i++) {
        CHECK(ptr_map_note(&m, &env, fake(i), X));
    }
    churn_map = &m;
    other     = 1000;
    ptr_map_sweep_env_t const e = {NULL, nolock, churn_unlock, count_free};
    CHECK(ptr_map_sweep(&m, X, &e) == 500);
    for (uintptr_t i = 1; i <= 500; i++) {
        CHECK(swept[i] == 1);
    }
    CHECK(m.used == 1); // only Y's last one is left
    CHECK(sweep(&m, Y) == 1);
    ptr_map_trim(&m, &env);
    CHECK(tables == 0);
}

// No memory for a bigger table: the pointer is not taken and the map is as it was
static void test_no_memory(void) {
    ptr_map_t m = {0};
    fail_alloc = 1;
    CHECK(!ptr_map_note(&m, &env, fake(1), X) && m.used == 0);
    fail_alloc = 0;
    for (uintptr_t i = 1; i <= 12; i++) {
        CHECK(ptr_map_note(&m, &env, fake(i), X)); // 12 of 16 slots: the next one needs a bigger table
    }
    fail_alloc = 1;
    CHECK(!ptr_map_note(&m, &env, fake(13), X) && m.used == 12 && m.cap == 16);
    CHECK(ptr_map_note(&m, &env, fake(3), Y)); // a known pointer needs no room
    CHECK(ptr_map_forget(&m, fake(12)));
    CHECK(ptr_map_note(&m, &env, fake(13), X)); // room again after a forget
    fail_alloc = 0;
    CHECK(sweep(&m, X) == 11 && sweep(&m, Y) == 1);
    ptr_map_trim(&m, &env);
    CHECK(tables == 0);
}

int main(void) {
    test_basic();
    test_forget_foreign();
    test_two_owners();
    test_reuse();
    test_churn_and_shift();
    test_sweep_with_churn();
    test_no_memory();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ptr_map_test ok\n");
    return 0;
}
