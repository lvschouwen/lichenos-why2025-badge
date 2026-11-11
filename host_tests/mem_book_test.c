// Host test: lichen_alloc's accounting (#181): per-app internal and PSRAM bytes, the internal quota (apps only, the
// OS is counted but never refused), the heap's rounding, and releases that never go below zero. Rows are keyed by the
// owner's context (#230 E1b), the label is only printed.
#include "mem_book.h"

#include <stdio.h>
#include <string.h>

static int failures;

// Stand-in contexts (lichen_ctx_t: slot + generation); the book only compares them
#define MESH  0x12345601u
#define WX    0x65432102u
#define BENCH 0x00abcd03u
#define OS    0x00777700u
#define NOBODY 0x00999909u

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
    CHECK(mem_book_find(&b, MESH) == NULL);
    CHECK(mem_book_charge(&b, MESH, "meshcore", false, true, 32 * 1024) == MEM_CHARGE_OK);
    CHECK(mem_book_charge(&b, MESH, "meshcore", false, true, 32 * 1024) == MEM_CHARGE_OK); // exactly the quota
    CHECK(mem_book_charge(&b, MESH, "meshcore", false, true, 1) == MEM_CHARGE_QUOTA);        // one byte over
    mem_use_t const *m = mem_book_find(&b, MESH);
    CHECK(m && m->internal == MEM_BOOK_INTERNAL_QUOTA && m->blocks == 2 && m->refused == 1);
    CHECK(mem_book_charge(&b, MESH, "meshcore", false, false, 4u * 1024 * 1024) == MEM_CHARGE_OK); // PSRAM has no quota
    CHECK(m->bulk == 4u * 1024 * 1024 && m->blocks == 3);
    // alone over the quota, and no overflow past the check
    CHECK(mem_book_charge(&b, WX, "weather", false, true, MEM_BOOK_INTERNAL_QUOTA + 1) == MEM_CHARGE_QUOTA);
    CHECK(mem_book_charge(&b, WX, "weather", false, true, UINT32_MAX) == MEM_CHARGE_QUOTA);
    CHECK(mem_book_charge(&b, WX, "weather", false, true, 1024) == MEM_CHARGE_OK); // another app has its own quota
    // The OS is never refused
    CHECK(mem_book_charge(&b, OS, NULL, true, true, 2 * MEM_BOOK_INTERNAL_QUOTA) == MEM_CHARGE_OK);
    CHECK(mem_book_find(&b, OS)->internal == 2 * MEM_BOOK_INTERNAL_QUOTA);
    CHECK(strcmp(mem_book_find(&b, OS)->owner, MEM_BOOK_OS) == 0 && mem_book_find(&b, OS)->os);
    // Free one: room again, the peak stays
    mem_book_release(&b, MESH, true, 32 * 1024);
    CHECK(m->internal == 32 * 1024 && m->internal_peak == MEM_BOOK_INTERNAL_QUOTA && m->blocks == 2);
    CHECK(mem_book_charge(&b, MESH, "meshcore", false, true, 16 * 1024) == MEM_CHARGE_OK);
}

static void test_adjust_release(void) {
    static mem_book_t b;
    memset(&b, 0, sizeof(b));
    CHECK(mem_book_charge(&b, BENCH, "bench", false, true, 100) == MEM_CHARGE_OK);
    mem_book_adjust(&b, BENCH, true, 4); // the heap gave 104
    mem_use_t const *r = mem_book_find(&b, BENCH);
    CHECK(r->internal == 104 && r->blocks == 1);
    CHECK(mem_book_release(&b, BENCH, true, 104) == MEM_RELEASE_OK);
    CHECK(r->internal == 0 && r->blocks == 0);
    CHECK(mem_book_release(&b, BENCH, true, 50) == MEM_RELEASE_OVER); // a stray free: reported, stays at zero
    CHECK(r->internal == 0 && r->blocks == 0);
    CHECK(mem_book_release(&b, NOBODY, false, 50) == MEM_RELEASE_NO_ROW); // never allocated: nothing made
    CHECK(mem_book_find(&b, NOBODY) == NULL);
    mem_book_adjust(&b, NOBODY, false, 8);
    CHECK(mem_book_find(&b, NOBODY) == NULL);
    // Owner drift: MeshCore's block freed as the weather app's, which holds less: reported, never below zero, and
    // MeshCore's own count stays up (what "fw mem" then shows)
    CHECK(mem_book_charge(&b, MESH, "meshcore", false, false, 4096) == MEM_CHARGE_OK);
    CHECK(mem_book_charge(&b, WX, "weather", false, false, 100) == MEM_CHARGE_OK);
    CHECK(mem_book_release(&b, WX, false, 4096) == MEM_RELEASE_OVER);
    CHECK(mem_book_find(&b, WX)->bulk == 0 && mem_book_find(&b, WX)->blocks == 0);
    CHECK(mem_book_find(&b, MESH)->bulk == 4096 && mem_book_find(&b, MESH)->blocks == 1);
    // Internal and PSRAM are separate: an internal free does not eat a PSRAM count
    CHECK(mem_book_release(&b, MESH, true, 4096) == MEM_RELEASE_OVER);
    CHECK(mem_book_find(&b, MESH)->bulk == 4096);
}

static void test_full_table(void) {
    static mem_book_t b;
    memset(&b, 0, sizeof(b));
    char id[8];
    for (int i = 0; i < MEM_BOOK_OWNERS; i++) {
        snprintf(id, sizeof(id), "app%d", i);
        CHECK(mem_book_charge(&b, 0x100u + (uint32_t)i, id, false, false, 10) == MEM_CHARGE_OK);
    }
    // A new owner finds no row: reported apart from the quota, so the caller can allocate unbooked
    uint32_t const late = 0x5000u;
    CHECK(mem_book_charge(&b, late, "late", false, false, 10) == MEM_CHARGE_NO_ROW);
    CHECK(mem_book_charge(&b, late, "late", false, true, 10) == MEM_CHARGE_NO_ROW);
    CHECK(mem_book_find(&b, late) == NULL);
    CHECK(mem_book_release(&b, late, false, 10) == MEM_RELEASE_NO_ROW); // its free: no row, nothing changes
    CHECK(mem_book_charge(&b, 0x103u, "app3", false, false, 10) == MEM_CHARGE_OK); // known owners still go
}

// #230 E1b: the context is the key, not the label. Two contexts with one label (yesterday's ctx of an app, or a
// made-up one that slipped past the gate) are two rows, so one cannot free or spend what the other holds; ctx 0 is
// never booked.
static void test_keyed_by_ctx(void) {
    static mem_book_t b;
    memset(&b, 0, sizeof(b));
    uint32_t const other = MESH ^ 0x100u; // the same slot, another generation
    CHECK(mem_book_charge(&b, MESH, "meshcore", false, true, MEM_BOOK_INTERNAL_QUOTA) == MEM_CHARGE_OK);
    CHECK(mem_book_charge(&b, other, "meshcore", false, true, 1024) == MEM_CHARGE_OK); // its own row and quota
    CHECK(mem_book_find(&b, MESH) != mem_book_find(&b, other));
    CHECK(mem_book_release(&b, other, true, MEM_BOOK_INTERNAL_QUOTA) == MEM_RELEASE_OVER);
    CHECK(mem_book_find(&b, MESH)->internal == MEM_BOOK_INTERNAL_QUOTA); // untouched by the other's free
    CHECK(mem_book_charge(&b, 0, "meshcore", false, false, 10) == MEM_CHARGE_NO_ROW);
    CHECK(mem_book_find(&b, 0) == NULL);
    CHECK(mem_book_release(&b, 0, false, 10) == MEM_RELEASE_NO_ROW);
    // The quota goes by the os flag, not the label: a row without a label that is not the OS's is limited
    CHECK(mem_book_charge(&b, BENCH, NULL, false, true, MEM_BOOK_INTERNAL_QUOTA + 1) == MEM_CHARGE_QUOTA);
    CHECK(strcmp(mem_book_find(&b, BENCH)->owner, MEM_BOOK_OS) == 0 && !mem_book_find(&b, BENCH)->os);
    // The label is set by the row's first charge; an over-long one is cut like an id
    CHECK(mem_book_charge(&b, WX, "an_overlong_owner_id", false, false, 1) == MEM_CHARGE_OK);
    CHECK(strlen(mem_book_find(&b, WX)->owner) == MEM_BOOK_OWNER_CAP - 1 && !mem_book_find(&b, WX)->os);
}

int main(void) {
    test_keyed_by_ctx();
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
