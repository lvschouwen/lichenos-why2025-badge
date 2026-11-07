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

    // p95 (#205): 100 frames, 94 at 10.x ms and 6 at 40 ms: the 95th sample is a 40 ms one -> upper edge 41
    perf_hist_t h = {0};
    CHECK(perf_hist_p95_ms(&h) == 0);
    for (int i = 0; i < 94; i++) {
        perf_hist_add(&h, 10500);
    }
    for (int i = 0; i < 6; i++) {
        perf_hist_add(&h, 40000);
    }
    CHECK(perf_hist_p95_ms(&h) == 41);
    // 95 fast of 100: the 95th sample is still fast -> 11
    perf_hist_t h2 = {0};
    for (int i = 0; i < 95; i++) {
        perf_hist_add(&h2, 10500);
    }
    for (int i = 0; i < 5; i++) {
        perf_hist_add(&h2, 80000);
    }
    CHECK(perf_hist_p95_ms(&h2) == 11);
    perf_hist_t h3 = {0};
    perf_hist_add(&h3, 5000000); // slower than the last bucket
    CHECK(perf_hist_p95_ms(&h3) == PERF_HIST_MAX_MS + 1);

    // The latency line: no keys yet, then keys
    ui_perf_lat_t lat = {0};
    ui_perf_format_latency(line, sizeof(line), &lat, 0);
    CHECK(strcmp(line, "ui perf latency: render p95 0 ms, key-to-flush none, first frame at none yet") == 0);
    perf_acc_add(&lat.key, 20000);
    perf_hist_add(&lat.key_hist, 20000);
    perf_acc_add(&lat.key, 40000);
    perf_hist_add(&lat.key_hist, 40000);
    perf_hist_add(&lat.render_hist, 12000);
    ui_perf_format_latency(line, sizeof(line), &lat, 1234);
    CHECK(strcmp(line, "ui perf latency: render p95 13 ms, key-to-flush 2 keys avg 30.0 max 40.0 p95 41 ms, first frame at 1234 ms") == 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ui_perf_stats_test ok\n");
    return 0;
}
