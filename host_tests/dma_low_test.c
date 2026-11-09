// Host test: when the DMA heap's low-water mark fell and who took the most then, for "fw mem" (#256)
#include "dma_low.h"

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

static char line[200];

static char const *at(dma_low_t const *l, unsigned i) {
    return dma_low_line(l, i, line, sizeof(line)) ? line : "";
}

static void first_and_steps(void) {
    dma_low_t l = {0};
    CHECK(dma_low_line(&l, 0, line, sizeof(line)) == 0); // nothing sampled yet

    CHECK(dma_low_feed(&l, 90000, 99000, 12345, 0, NULL));
    CHECK(dma_low_line(&l, 0, line, sizeof(line)) == strlen(line));
    CHECK(strcmp(line, "mem dma_low first min 90000 fell 0 at 12.345s free 99000 big 0 task -") == 0);
    CHECK(dma_low_line(&l, 1, line, sizeof(line)) == 0);

    // The same mark, or a higher one (never from the heap), changes nothing
    CHECK(!dma_low_feed(&l, 90000, 99000, 12600, 0, "c6sup"));
    CHECK(!dma_low_feed(&l, 95000, 99000, 12700, 0, "c6sup"));
    CHECK(strcmp(at(&l, 0), "mem dma_low first min 90000 fell 0 at 12.345s free 99000 big 0 task -") == 0);

    // A fall below the step is no record, but the newest one follows it down (its moment stays) and takes the
    // window's allocation when it is the largest so far
    CHECK(!dma_low_feed(&l, 89000, 98500, 12850, 1200, "c6sup"));
    CHECK(strcmp(at(&l, 0), "mem dma_low first min 89000 fell 1000 at 12.345s free 99000 big 1200 task c6sup") == 0);

    // A step below where the newest record started (90000) starts one; an empty task name reads "-"
    CHECK(dma_low_feed(&l, 89000 - DMA_LOW_STEP, 98000, 13100, 2048, ""));
    CHECK(strcmp(at(&l, 1), "mem dma_low drop 1/1 min 86952 fell 2048 at 13.100s free 98000 big 2048 task -") == 0);

    // A 32 KB peak hours later: the uptime survives 32-bit milliseconds
    CHECK(dma_low_feed(&l, 54184, 70000, 5077000000ull, 1556, "ota_shot"));
    CHECK(strcmp(at(&l, 2),
                 "mem dma_low drop 2/2 min 54184 fell 32768 at 5077000.000s free 70000 big 1556 task ota_shot") == 0);
}

// Seven drops of 4096 each from 100000, then the ring holds what it should after `drops` of them
static void feed_drops(dma_low_t *l, int drops) {
    *l = (dma_low_t){0};
    dma_low_feed(l, 100000, 100000, 0, 0, "main");
    for (int i = 1; i <= drops; i++) {
        CHECK(dma_low_feed(l, 100000 - (uint32_t)i * 4096, 100000, (uint64_t)i * 1000, (uint32_t)i, "t"));
    }
    CHECK(l->drops == (uint32_t)drops);
}

static void ring_wraps(void) {
    dma_low_t l;
    // Exactly full: drops 1..4, nothing lost yet
    feed_drops(&l, DMA_LOW_KEEP);
    CHECK(strstr(at(&l, 0), "first min 100000 fell 0 ") != NULL);
    CHECK(strstr(at(&l, 1), "drop 1/4 min 95904 fell 4096 at 1.000s free 100000 big 1 ") != NULL);
    CHECK(strstr(at(&l, 4), "drop 4/4 min 83616 fell 4096 at 4.000s free 100000 big 4 ") != NULL);
    CHECK(dma_low_line(&l, 5, line, sizeof(line)) == 0);

    // One past full: drop 1 goes, 2..5 stay, the first sample stays
    feed_drops(&l, DMA_LOW_KEEP + 1);
    CHECK(strstr(at(&l, 0), "first min 100000 ") != NULL);
    CHECK(strstr(at(&l, 1), "drop 2/5 min 91808 fell 4096 at 2.000s") != NULL);
    CHECK(strstr(at(&l, 4), "drop 5/5 min 79520 fell 4096 at 5.000s") != NULL);
    CHECK(dma_low_line(&l, 5, line, sizeof(line)) == 0);

    // A sub-step fall after the wrap updates the newest (drop 5), not a stale slot
    CHECK(!dma_low_feed(&l, 79000, 90000, 6000, 0, "x"));
    CHECK(strstr(at(&l, 4), "drop 5/5 min 79000 fell 4616 at 5.000s") != NULL);
    CHECK(strstr(at(&l, 3), "drop 4/5 min 83616 fell 4096 ") != NULL);
    // ...and the next step is measured from where drop 5 started (79520), not from its moving min
    CHECK(dma_low_feed(&l, 79520 - DMA_LOW_STEP, 90000, 6250, 0, "x"));
    CHECK(strstr(at(&l, 4), "drop 6/6 min 77472 fell 1528 at 6.250s") != NULL);
}

// The #256 case: TX frame copies queue up ~1 KB per sample. No single sample falls a step, the sum does
static void small_falls_add_up(void) {
    dma_low_t l = {0};
    dma_low_feed(&l, 100000, 100000, 0, 0, "main");
    CHECK(!dma_low_feed(&l, 99000, 99000, 250, 1556, "ota"));
    CHECK(!dma_low_feed(&l, 98000, 98000, 500, 1556, "ota"));
    CHECK(strstr(at(&l, 0), "first min 98000 fell 2000 at 0.000s free 100000 big 1556 task ota") != NULL);
    CHECK(dma_low_feed(&l, 97000, 97000, 750, 1556, "ota_shot")); // 3000 below where the record started
    CHECK(strcmp(at(&l, 1), "mem dma_low drop 1/1 min 97000 fell 1000 at 0.750s free 97000 big 1556 task ota_shot") == 0);
    // ...and on: 17 KB more in 1 KB falls gives a record every 3 KB (2 KB is not yet a step), the newest at the true
    // minimum
    for (uint32_t m = 96000; m >= 80000; m -= 1000) {
        dma_low_feed(&l, m, m, 1000 + (100000 - m) / 4, 1556, "ota_shot");
    }
    CHECK(l.drops == 6);
    CHECK(strstr(at(&l, DMA_LOW_KEEP), "drop 6/6 min 80000 ") != NULL);
    CHECK(strstr(at(&l, DMA_LOW_KEEP), " big 1556 task ota_shot") != NULL);
}

static void long_task_name_and_short_buffer(void) {
    dma_low_t l = {0};
    char      small[24];
    dma_low_feed(&l, 1000, 2000, 0, 0, "a_task_name_longer_than_sixteen");
    CHECK(strlen(l.first.task) == DMA_LOW_TASK_CAP - 1);
    size_t n = dma_low_line(&l, 0, small, sizeof(small));
    CHECK(n > sizeof(small) && strlen(small) == sizeof(small) - 1); // snprintf's length, cut safely
    CHECK(dma_low_line(&l, 0, small, 0) == 0);
}

int main(void) {
    first_and_steps();
    ring_wraps();
    small_falls_add_up();
    long_task_name_and_short_buffer();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("dma_low_test ok\n");
    return 0;
}
