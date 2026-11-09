// Host test: when the DMA heap's low-water mark fell, for "fw mem" (#256)
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

static void first_and_steps(void) {
    dma_low_t l = {0};
    char      line[160];
    CHECK(dma_low_line(&l, 0, line, sizeof(line)) == 0); // nothing sampled yet

    CHECK(dma_low_feed(&l, 90000, 99000, 12345, NULL));
    CHECK(dma_low_line(&l, 0, line, sizeof(line)) == strlen(line));
    CHECK(strcmp(line, "mem dma_low first min 90000 fell 0 at 12.345s free 99000 task -") == 0);
    CHECK(dma_low_line(&l, 1, line, sizeof(line)) == 0);

    // Churn below the step is not a record, the same mark again neither
    CHECK(!dma_low_feed(&l, 90000, 99000, 12600, "c6sup"));
    CHECK(!dma_low_feed(&l, 90000 - DMA_LOW_STEP + 1, 99000, 12850, "c6sup"));
    // Small steps that add up to one are a record, measured from the last record
    CHECK(dma_low_feed(&l, 90000 - DMA_LOW_STEP, 98000, 13100, "ota"));
    CHECK(dma_low_line(&l, 1, line, sizeof(line)) > 0);
    CHECK(strcmp(line, "mem dma_low drop 1/1 min 87952 fell 2048 at 13.100s free 98000 task ota") == 0);

    // A 32 KB peak hours later: the uptime survives 32-bit milliseconds
    CHECK(dma_low_feed(&l, 55184, 70000, 5077000000ull, "ota_shot"));
    CHECK(dma_low_line(&l, 2, line, sizeof(line)) > 0);
    CHECK(strcmp(line, "mem dma_low drop 2/2 min 55184 fell 32768 at 5077000.000s free 70000 task ota_shot") == 0);
}

static void ring_keeps_the_newest(void) {
    dma_low_t l = {0};
    char      line[160];
    dma_low_feed(&l, 100000, 100000, 0, "main");
    for (int i = 1; i <= 7; i++) {
        CHECK(dma_low_feed(&l, 100000 - (uint32_t)i * 4096, 100000, (uint64_t)i * 1000, "t"));
    }
    CHECK(l.drops == 7);
    // The first sample stays; the last DMA_LOW_KEEP drops follow, oldest first, each with its own fall
    CHECK(dma_low_line(&l, 0, line, sizeof(line)) > 0 && strstr(line, "first min 100000 ") != NULL);
    CHECK(dma_low_line(&l, 1, line, sizeof(line)) > 0 && strstr(line, "drop 4/7 min 83616 fell 4096 at 4.000s") != NULL);
    CHECK(dma_low_line(&l, DMA_LOW_KEEP, line, sizeof(line)) > 0 &&
          strstr(line, "drop 7/7 min 71328 fell 4096 at 7.000s") != NULL);
    CHECK(dma_low_line(&l, DMA_LOW_KEEP + 1, line, sizeof(line)) == 0);
}

static void long_task_name_and_short_buffer(void) {
    dma_low_t l = {0};
    char      line[24];
    dma_low_feed(&l, 1000, 2000, 0, "a_task_name_longer_than_sixteen");
    CHECK(strlen(l.first.task) == DMA_LOW_TASK_CAP - 1);
    size_t n = dma_low_line(&l, 0, line, sizeof(line));
    CHECK(n > sizeof(line) && strlen(line) == sizeof(line) - 1); // snprintf's length, cut safely
    CHECK(dma_low_line(&l, 0, line, 0) == 0);
}

int main(void) {
    first_and_steps();
    ring_keeps_the_newest();
    long_task_name_and_short_buffer();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("dma_low_test ok\n");
    return 0;
}
