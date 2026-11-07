// Host test: the "fw mem" heap line with totals and percentages (#205)
#include "mem_fmt.h"

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

int main(void) {
    char   line[200];
    size_t n = mem_fmt_heap(line, sizeof(line), "internal", 123000, 100000, 80000, 400000);
    CHECK(n == strlen(line));
    CHECK(strcmp(line, "mem internal free 123000 largest 100000 min 80000 total 400000 free_pct 30 min_pct 20 largest_pct 81 "
                       "used_pct 69") == 0);

    // Empty or unknown heap: no division by zero
    mem_fmt_heap(line, sizeof(line), "dma", 0, 0, 0, 0);
    CHECK(strcmp(line, "mem dma free 0 largest 0 min 0 total 0 free_pct 0 min_pct 0 largest_pct 0 used_pct 0") == 0);

    // Above 4 GiB / 100 would overflow 32-bit maths: PSRAM-sized values stay exact
    mem_fmt_heap(line, sizeof(line), "psram", 30000000, 20000000, 25000000, 33554432);
    CHECK(strstr(line, "total 33554432 free_pct 89 min_pct 74 largest_pct 66 used_pct 10") != NULL);

    // Inconsistent input (free above total) is clamped, not wrapped
    mem_fmt_heap(line, sizeof(line), "x", 500, 500, 500, 100);
    CHECK(strstr(line, "free_pct 100 min_pct 100 largest_pct 100 used_pct 0") != NULL);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mem_fmt_test ok\n");
    return 0;
}
