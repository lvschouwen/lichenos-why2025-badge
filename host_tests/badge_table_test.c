// Host test: the tile badge count behind lichen_badge.h (#102, #520)
#include "badge_table.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    uint32_t crashlog = 0, mesh = 0, generation = 0;
    // Set, change, clear; the generation moves only on a change, and a count is its app's own
    CHECK(!badge_count_set(&crashlog, &generation, 0) && generation == 0); // none stays none
    CHECK(badge_count_set(&crashlog, &generation, 1) && crashlog == 1 && generation == 1);
    CHECK(!badge_count_set(&crashlog, &generation, 1) && generation == 1); // the same count again
    CHECK(badge_count_set(&mesh, &generation, 120) && mesh == 120 && generation == 2);
    CHECK(crashlog == 1 && mesh == 120);
    CHECK(badge_count_set(&crashlog, &generation, 3) && generation == 3);
    CHECK(badge_count_set(&crashlog, &generation, 0) && crashlog == 0 && generation == 4);
    CHECK(mesh == 120);
    // The generation wraps like any counter
    uint32_t last = 0, wrap = UINT32_MAX;
    CHECK(badge_count_set(&last, &wrap, 5) && wrap == 0);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("badge_table_test: ok\n");
    return 0;
}
