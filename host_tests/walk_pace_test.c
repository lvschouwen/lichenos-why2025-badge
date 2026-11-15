// Host test: the slicing rule of a directory walk (#471, walk_pace.h). A walk of 360 entries at 15 ms each (a stat in
// a big LittleFS folder) held core 0 for 5.4 s without a block; sliced, no stretch passes the budget plus one entry.
#include "walk_pace.h"

#include <stdio.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// Runs n entries of cost_us each through the rule; a block takes block_us. Returns the walk's worst stretch.
static uint32_t walk(unsigned n, int64_t cost_us, int64_t block_us, uint32_t *yields) {
    walk_slice_t s;
    int64_t      now = 1000000;
    walk_slice_begin(&s, now);
    for (unsigned i = 0; i < n; i++) {
        now += cost_us;
        if (walk_slice_step(&s, now)) {
            now += block_us;
            walk_slice_resume(&s, now);
        }
    }
    walk_slice_step(&s, now);
    *yields = s.yields;
    return s.worst_us;
}

int main(void) {
    uint32_t yields;
    // The soak's walk: 360 stats of 15 ms. Unsliced that is one 5.4 s stretch; sliced every stretch ends within one
    // entry of the budget, far under the 100 ms the issue allows
    uint32_t worst = walk(360, 15000, 1000, &yields);
    CHECK(worst < WALK_SLICE_US + 15000);
    CHECK(worst <= 100000);
    CHECK(yields == 180); // a block every second entry

    // Cheap entries: a block every WALK_SLICE_US, never per entry
    worst = walk(1000, 100, 1000, &yields);
    CHECK(worst >= WALK_SLICE_US && worst < WALK_SLICE_US + 100);
    CHECK(yields == 1000 * 100 / WALK_SLICE_US);

    // A short walk never blocks, and its whole length is its worst stretch
    worst = walk(10, 500, 1000, &yields);
    CHECK(yields == 0 && worst == 5000);

    // One slow entry is a stretch of its own: the worst shows it, the next stretch starts after the block
    walk_slice_t s;
    walk_slice_begin(&s, 0);
    CHECK(walk_slice_step(&s, 80000));
    walk_slice_resume(&s, 81000);
    CHECK(!walk_slice_step(&s, 82000));
    CHECK(s.worst_us == 80000 && s.yields == 1);

    // A clock that goes back counts nothing and starts the stretch over
    walk_slice_begin(&s, 50000);
    CHECK(!walk_slice_step(&s, 10000));
    CHECK(s.worst_us == 0);
    CHECK(walk_slice_step(&s, 10000 + WALK_SLICE_US));

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("walk_pace_test ok\n");
    return 0;
}
