#include "mem_diff.h"

#include <stdlib.h>
#include <string.h>

static int by_addr(void const *a, void const *b) {
    uint32_t x = ((mem_blk_t const *)a)->addr;
    uint32_t y = ((mem_blk_t const *)b)->addr;
    return x < y ? -1 : x > y;
}

void mem_blk_sort(mem_blk_t *v, size_t n) {
    if (n > 1) {
        qsort(v, n, sizeof(*v), by_addr);
    }
}

static void note(mem_diff_sum_t *sum, mem_diff_fn fn, void *user, mem_diff_kind_t kind, mem_blk_t const *was,
                 mem_blk_t const *now) {
    switch (kind) {
        case MEM_DIFF_ADDED:
            sum->added_n++;
            sum->added_bytes += now->size;
            sum->net += now->size;
            break;
        case MEM_DIFF_REMOVED:
            sum->removed_n++;
            sum->removed_bytes += was->size;
            sum->net -= was->size;
            break;
        case MEM_DIFF_RESIZED:
            sum->resized_n++;
            sum->net += (int64_t)now->size - (int64_t)was->size;
            break;
    }
    if (fn) {
        fn(kind, was, now, user);
    }
}

void mem_diff(mem_blk_t const *was, size_t was_n, mem_blk_t const *now, size_t now_n, mem_diff_sum_t *sum, mem_diff_fn fn,
              void *user) {
    memset(sum, 0, sizeof(*sum));
    size_t i = 0, j = 0;
    while (i < was_n || j < now_n) {
        if (j >= now_n || (i < was_n && was[i].addr < now[j].addr)) {
            note(sum, fn, user, MEM_DIFF_REMOVED, &was[i++], NULL);
        } else if (i >= was_n || now[j].addr < was[i].addr) {
            note(sum, fn, user, MEM_DIFF_ADDED, NULL, &now[j++]);
        } else {
            if (was[i].size != now[j].size) {
                note(sum, fn, user, MEM_DIFF_RESIZED, &was[i], &now[j]);
            }
            i++;
            j++;
        }
    }
}
