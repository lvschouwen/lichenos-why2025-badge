// Host test: the Bench app's arithmetic and result lines (#96)
#include "bench_calc.h"

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
    char line[96];

    // 1 MB (1048576 B) in 3355 us = 298.06 MB/s; 1 MB in 1 s = 1.0 MB/s; nothing measured = "-"
    CHECK(bench_rate_x10(BENCH_MB, 3355) == 2981);
    CHECK(bench_rate_x10(BENCH_MB, 1000000) == 10);
    CHECK(bench_rate_x10(BENCH_MB, 0) == 0);
    bench_fmt_rate(line, sizeof(line), BENCH_MB, 3355);
    CHECK(strcmp(line, "298.1 MB/s") == 0);
    bench_fmt_rate(line, sizeof(line), 32 * BENCH_MB, 100000); // 32 MB in 0.1 s
    CHECK(strcmp(line, "320.0 MB/s") == 0);
    CHECK(bench_fmt_rate(line, sizeof(line), BENCH_MB, 0) == 1 && strcmp(line, "-") == 0);

    // 10 frames in 931 ms, 90 strips flushed in 531 ms
    bench_fmt_redraw(line, sizeof(line), 931000, 10, 531000, 90);
    CHECK(strcmp(line, "93.1 ms/frame, flush 5.9 ms/strip") == 0);
    bench_fmt_redraw(line, sizeof(line), 931000, 10, 0, 0);
    CHECK(strcmp(line, "93.1 ms/frame") == 0);
    bench_fmt_redraw(line, sizeof(line), 0, 0, 0, 0);
    CHECK(strcmp(line, "-") == 0);

    // 30 frames in 3000 ms, renders of 78.9 ms
    bench_fmt_scroll(line, sizeof(line), 30, 3000, 78900);
    CHECK(strcmp(line, "10.0 fps, render 79 ms") == 0);
    bench_fmt_scroll(line, sizeof(line), 0, 3000, 0);
    CHECK(strcmp(line, "-") == 0);

    bench_fmt_mem(line, sizeof(line), 155091, 146519, 115591, 27883616);
    CHECK(strcmp(line, "int 151 KB (min 143), DMA 112 KB, PSRAM 26.6 MB") == 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("bench_calc_test ok\n");
    return 0;
}
