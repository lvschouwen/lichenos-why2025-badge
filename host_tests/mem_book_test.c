// Host test: lichen_alloc's accounting (#181): per-app internal and PSRAM bytes, the internal quota (apps only, the
// OS is counted but never refused), the heap's rounding, and releases that never go below zero.
#include "mem_book.h"

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

static void test_quota(void) {
    static mem_book_t b;
    memset(&b, 0, sizeof(b));
    CHECK(mem_book_find(&b, "meshcore") == NULL);
    CHECK(mem_book_charge(&b, "meshcore", true, 32 * 1024));
    CHECK(mem_book_charge(&b, "meshcore", true, 32 * 1024)); // exactly the quota
    CHECK(!mem_book_charge(&b, "meshcore", true, 1));        // one byte over
    mem_use_t const *m = mem_book_find(&b, "meshcore");
    CHECK(m && m->internal == MEM_BOOK_INTERNAL_QUOTA && m->blocks == 2 && m->refused == 1);
    CHECK(mem_book_charge(&b, "meshcore", false, 4u * 1024 * 1024)); // PSRAM has no quota
    CHECK(m->bulk == 4u * 1024 * 1024 && m->blocks == 3);
    CHECK(!mem_book_charge(&b, "weather", true, MEM_BOOK_INTERNAL_QUOTA + 1)); // alone over the quota
    CHECK(!mem_book_charge(&b, "weather", true, UINT32_MAX));                  // no overflow past the check
    CHECK(mem_book_charge(&b, "weather", true, 1024)); // another app has its own quota
    // The OS is never refused
    CHECK(mem_book_charge(&b, NULL, true, 2 * MEM_BOOK_INTERNAL_QUOTA));
    CHECK(mem_book_find(&b, MEM_BOOK_OS)->internal == 2 * MEM_BOOK_INTERNAL_QUOTA);
    // Free one: room again, the peak stays
    mem_book_release(&b, "meshcore", true, 32 * 1024);
    CHECK(m->internal == 32 * 1024 && m->internal_peak == MEM_BOOK_INTERNAL_QUOTA && m->blocks == 2);
    CHECK(mem_book_charge(&b, "meshcore", true, 16 * 1024));
}

static void test_adjust_release(void) {
    static mem_book_t b;
    memset(&b, 0, sizeof(b));
    CHECK(mem_book_charge(&b, "bench", true, 100));
    mem_book_adjust(&b, "bench", true, 4); // the heap gave 104
    mem_use_t const *r = mem_book_find(&b, "bench");
    CHECK(r->internal == 104 && r->blocks == 1);
    mem_book_release(&b, "bench", true, 104);
    CHECK(r->internal == 0 && r->blocks == 0);
    mem_book_release(&b, "bench", true, 50); // a stray free: stays at zero
    CHECK(r->internal == 0 && r->blocks == 0);
    mem_book_release(&b, "nobody", false, 50); // never allocated: nothing made
    CHECK(mem_book_find(&b, "nobody") == NULL);
    mem_book_adjust(&b, "nobody", false, 8);
    CHECK(mem_book_find(&b, "nobody") == NULL);
}

static void test_full_table(void) {
    static mem_book_t b;
    memset(&b, 0, sizeof(b));
    char id[8];
    for (int i = 0; i < MEM_BOOK_OWNERS; i++) {
        snprintf(id, sizeof(id), "app%d", i);
        CHECK(mem_book_charge(&b, id, false, 10));
    }
    CHECK(!mem_book_charge(&b, "late", false, 10));
    CHECK(mem_book_charge(&b, "app3", false, 10)); // known owners still go
}

int main(void) {
    test_quota();
    test_adjust_release();
    test_full_table();
    if (failures) {
        printf("mem_book_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("mem_book_test: ok\n");
    return 0;
}
