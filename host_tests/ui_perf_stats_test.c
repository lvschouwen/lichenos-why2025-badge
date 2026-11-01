// Host test: LVGL frame timing accumulation and the "fw ui perf" line (#93)
#include "ui_perf_stats.h"

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
    ui_perf_t p;
    char      line[200];
    ui_perf_reset(&p, 5000);
    CHECK(perf_acc_avg_us(&p.render) == 0);
    ui_perf_format(line, sizeof(line), &p, 35000);
    CHECK(strcmp(line, "ui perf: no frames in 30.0 s") == 0);

    // Three renders of 40, 50 and 90 ms: average 60.0, max 90.0; nine flushes of 3.1 ms; waits 2.0 and 8.14 ms
    perf_acc_add(&p.render, 40000);
    perf_acc_add(&p.render, 50000);
    perf_acc_add(&p.render, 90000);
    CHECK(p.render.n == 3 && perf_acc_avg_us(&p.render) == 60000 && p.render.max_us == 90000);
    for (int i = 0; i < 9; i++) {
        perf_acc_add(&p.flush, 3100);
    }
    perf_acc_add(&p.wait, 2000);
    perf_acc_add(&p.wait, 8140);
    size_t n = ui_perf_format(line, sizeof(line), &p, 15000); // 10 s: 0.3 frames per second
    CHECK(n == strlen(line));
    CHECK(strcmp(line, "ui perf: 3 frames in 10.0 s (0.3/s), render avg 60.0 max 90.0 ms, 9 flushes avg 3.1 max 3.1 ms, "
                       "flush wait avg 5.1 max 8.1 ms") == 0);

    // A reset starts over from the given time
    ui_perf_reset(&p, 15000);
    CHECK(p.render.n == 0 && p.flush.n == 0 && p.wait.n == 0 && p.since_ms == 15000);
    ui_perf_format(line, sizeof(line), &p, 15500);
    CHECK(strcmp(line, "ui perf: no frames in 0.5 s") == 0);

    // Uptime wrap: the span is unsigned arithmetic
    ui_perf_reset(&p, 0xFFFFFF00u);
    perf_acc_add(&p.render, 1000);
    ui_perf_format(line, sizeof(line), &p, 0x00000100u);
    CHECK(strncmp(line, "ui perf: 1 frames in 0.5 s", 26) == 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ui_perf_stats_test ok\n");
    return 0;
}
