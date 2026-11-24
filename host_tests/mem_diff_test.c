// Host test: the block-list comparison behind "fw mem diff" (#547)
#include "mem_diff.h"

#include <stdio.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

typedef struct {
    int      n;
    unsigned seen[8]; // kind << 28 | addr
} log_t;

static void on_diff(mem_diff_kind_t kind, mem_blk_t const *was, mem_blk_t const *now, void *user) {
    log_t *l = user;
    if (l->n < 8) {
        l->seen[l->n++] = ((unsigned)kind << 28) | (kind == MEM_DIFF_ADDED ? now->addr : was->addr);
    }
}

int main(void) {
    // Unsorted input (heaps are walked one after the other) is ordered by address first
    mem_blk_t was[] = {{0x3000, 64}, {0x1000, 100}, {0x2000, 200}, {0x4000, 8}};
    mem_blk_t now[] = {{0x4000, 8}, {0x1000, 100}, {0x2000, 300}, {0x5000, 16384}, {0x6000, 32}};
    mem_blk_sort(was, 4);
    mem_blk_sort(now, 5);
    CHECK(was[0].addr == 0x1000 && was[3].addr == 0x4000);

    log_t          l = {0};
    mem_diff_sum_t s;
    mem_diff(was, 4, now, 5, &s, on_diff, &l);
    CHECK(s.added_n == 2 && s.added_bytes == 16384 + 32);
    CHECK(s.removed_n == 1 && s.removed_bytes == 64);
    CHECK(s.resized_n == 1);
    CHECK(s.net == 16384 + 32 - 64 + 100);
    CHECK(l.n == 4);
    CHECK(l.seen[0] == (((unsigned)MEM_DIFF_RESIZED << 28) | 0x2000)); // address order
    CHECK(l.seen[1] == (((unsigned)MEM_DIFF_REMOVED << 28) | 0x3000));
    CHECK(l.seen[2] == (((unsigned)MEM_DIFF_ADDED << 28) | 0x5000));

    // Same list: nothing; empty lists and a NULL callback are fine
    mem_diff(now, 5, now, 5, &s, NULL, NULL);
    CHECK(s.added_n == 0 && s.removed_n == 0 && s.resized_n == 0 && s.net == 0);
    mem_diff(NULL, 0, now, 5, &s, NULL, NULL);
    CHECK(s.added_n == 5);
    mem_diff(now, 5, NULL, 0, &s, NULL, NULL);
    CHECK(s.removed_n == 5 && s.net == -(8 + 100 + 300 + 16384 + 32));
    mem_blk_sort(NULL, 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mem_diff_test ok\n");
    return 0;
}
