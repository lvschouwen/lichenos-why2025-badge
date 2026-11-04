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
    CHECK(mem_book_charge(&b, "meshcore", true, 32 * 1024) == MEM_CHARGE_OK);
    CHECK(mem_book_charge(&b, "meshcore", true, 32 * 1024) == MEM_CHARGE_OK); // exactly the quota
    CHECK(mem_book_charge(&b, "meshcore", true, 1) == MEM_CHARGE_QUOTA);        // one byte over
    mem_use_t const *m = mem_book_find(&b, "meshcore");
    CHECK(m && m->internal == MEM_BOOK_INTERNAL_QUOTA && m->blocks == 2 && m->refused == 1);
    CHECK(mem_book_charge(&b, "meshcore", false, 4u * 1024 * 1024) == MEM_CHARGE_OK); // PSRAM has no quota
    CHECK(m->bulk == 4u * 1024 * 1024 && m->blocks == 3);
    CHECK(mem_book_charge(&b, "weather", true, MEM_BOOK_INTERNAL_QUOTA + 1) == MEM_CHARGE_QUOTA); // alone over the quota
    CHECK(mem_book_charge(&b, "weather", true, UINT32_MAX) == MEM_CHARGE_QUOTA);                  // no overflow past the check
    CHECK(mem_book_charge(&b, "weather", true, 1024) == MEM_CHARGE_OK); // another app has its own quota
    // The OS is never refused
    CHECK(mem_book_charge(&b, NULL, true, 2 * MEM_BOOK_INTERNAL_QUOTA) == MEM_CHARGE_OK);
    CHECK(mem_book_find(&b, MEM_BOOK_OS)->internal == 2 * MEM_BOOK_INTERNAL_QUOTA);
    // Free one: room again, the peak stays
    mem_book_release(&b, "meshcore", true, 32 * 1024);
    CHECK(m->internal == 32 * 1024 && m->internal_peak == MEM_BOOK_INTERNAL_QUOTA && m->blocks == 2);
    CHECK(mem_book_charge(&b, "meshcore", true, 16 * 1024) == MEM_CHARGE_OK);
}

static void test_adjust_release(void) {
    static mem_book_t b;
    memset(&b, 0, sizeof(b));
    CHECK(mem_book_charge(&b, "bench", true, 100) == MEM_CHARGE_OK);
    mem_book_adjust(&b, "bench", true, 4); // the heap gave 104
    mem_use_t const *r = mem_book_find(&b, "bench");
    CHECK(r->internal == 104 && r->blocks == 1);
    CHECK(mem_book_release(&b, "bench", true, 104) == MEM_RELEASE_OK);
    CHECK(r->internal == 0 && r->blocks == 0);
    CHECK(mem_book_release(&b, "bench", true, 50) == MEM_RELEASE_OVER); // a stray free: reported, stays at zero
    CHECK(r->internal == 0 && r->blocks == 0);
    CHECK(mem_book_release(&b, "nobody", false, 50) == MEM_RELEASE_NO_ROW); // never allocated: nothing made
    CHECK(mem_book_find(&b, "nobody") == NULL);
    mem_book_adjust(&b, "nobody", false, 8);
    CHECK(mem_book_find(&b, "nobody") == NULL);
    // Owner drift: MeshCore's block freed as the weather app's, which holds less: reported, never below zero, and
    // MeshCore's own count stays up (what "fw mem" then shows)
    CHECK(mem_book_charge(&b, "meshcore", false, 4096) == MEM_CHARGE_OK);
    CHECK(mem_book_charge(&b, "weather", false, 100) == MEM_CHARGE_OK);
    CHECK(mem_book_release(&b, "weather", false, 4096) == MEM_RELEASE_OVER);
    CHECK(mem_book_find(&b, "weather")->bulk == 0 && mem_book_find(&b, "weather")->blocks == 0);
    CHECK(mem_book_find(&b, "meshcore")->bulk == 4096 && mem_book_find(&b, "meshcore")->blocks == 1);
    // Internal and PSRAM are separate: an internal free does not eat a PSRAM count
    CHECK(mem_book_release(&b, "meshcore", true, 4096) == MEM_RELEASE_OVER);
    CHECK(mem_book_find(&b, "meshcore")->bulk == 4096);
}

static void test_full_table(void) {
    static mem_book_t b;
    memset(&b, 0, sizeof(b));
    char id[8];
    for (int i = 0; i < MEM_BOOK_OWNERS; i++) {
        snprintf(id, sizeof(id), "app%d", i);
        CHECK(mem_book_charge(&b, id, false, 10) == MEM_CHARGE_OK);
    }
    // A new owner finds no row: reported apart from the quota, so the caller can allocate unbooked
    CHECK(mem_book_charge(&b, "late", false, 10) == MEM_CHARGE_NO_ROW);
    CHECK(mem_book_charge(&b, "late", true, 10) == MEM_CHARGE_NO_ROW);
    CHECK(mem_book_find(&b, "late") == NULL);
    CHECK(mem_book_release(&b, "late", false, 10) == MEM_RELEASE_NO_ROW); // its free: no row, nothing changes
    CHECK(mem_book_charge(&b, "app3", false, 10) == MEM_CHARGE_OK); // known owners still go
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
