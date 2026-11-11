// Host test: the Stress app's rules and log lines (#278)
#include "stress_calc.h"

#include <stdint.h>
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

static stress_sample_t sample(uint32_t base) {
    return (stress_sample_t){.dma_free    = base + 1,
                             .dma_min     = base + 2,
                             .dma_largest = base + 3,
                             .int_free    = base + 4,
                             .int_min     = base + 5,
                             .int_largest = base + 6,
                             .psram_free  = base + 7,
                             .pool_peak   = 8,
                             .pool_drop   = 0,
                             .lat_ms      = 3};
}

// A clean observation: every rule passes
static stress_obs_t clean(void) {
    return (stress_obs_t){.s = sample(100000), .heap_ok = true, .stack_low = UINT32_MAX};
}

static void test_parse(void) {
    unsigned m = 0;
    CHECK(stress_parse("", &m) == STRESS_CMD_START && m == STRESS_MIN_DEFAULT);
    CHECK(stress_parse(" ", &m) == STRESS_CMD_START && m == 10);
    CHECK(stress_parse(NULL, &m) == STRESS_CMD_START && m == 10);
    CHECK(stress_parse(" 1", &m) == STRESS_CMD_START && m == 1);
    CHECK(stress_parse("30", &m) == STRESS_CMD_START && m == 30);
    CHECK(stress_parse(" 5 ", &m) == STRESS_CMD_START && m == 5);
    CHECK(stress_parse("0", &m) == STRESS_CMD_BAD);
    CHECK(stress_parse("31", &m) == STRESS_CMD_BAD);
    CHECK(stress_parse("-3", &m) == STRESS_CMD_BAD);
    CHECK(stress_parse("+3", &m) == STRESS_CMD_BAD);
    CHECK(stress_parse("99999999999999999999", &m) == STRESS_CMD_BAD);
    CHECK(stress_parse("stop", &m) == STRESS_CMD_STOP);
    CHECK(stress_parse(" stop ", &m) == STRESS_CMD_STOP);
    CHECK(stress_parse("status", &m) == STRESS_CMD_STATUS);
    CHECK(stress_parse("stopx", &m) == STRESS_CMD_BAD);
    CHECK(stress_parse("stop 5", &m) == STRESS_CMD_BAD);
    CHECK(stress_parse("5x", &m) == STRESS_CMD_BAD);
    CHECK(stress_parse("5 6", &m) == STRESS_CMD_BAD);
    CHECK(stress_parse("garbage", &m) == STRESS_CMD_BAD);
}

static void test_rules_alone(void) {
    stress_obs_t o = clean();
    CHECK(stress_judge(&o) == STRESS_NONE);
    o = clean(), o.mismatches = 1;
    CHECK(stress_judge(&o) == STRESS_MISMATCH);
    o = clean(), o.dma_bad = 1;
    CHECK(stress_judge(&o) == STRESS_DMA_BAD);
    o = clean(), o.heap_ok = false;
    CHECK(stress_judge(&o) == STRESS_HEAP);
    o = clean(), o.alloc_fails = 2;
    CHECK(stress_judge(&o) == STRESS_ALLOC);
    o = clean(), o.lock_hits = 1;
    CHECK(stress_judge(&o) == STRESS_LOCKCHECK);
    o = clean(), o.s.lat_ms = STRESS_LIMIT_LAT_MS;
    CHECK(stress_judge(&o) == STRESS_NONE); // at the limit is still fine
    o.s.lat_ms = STRESS_LIMIT_LAT_MS + 1;
    CHECK(stress_judge(&o) == STRESS_LATENCY);
    o = clean(), o.s.dma_free = STRESS_LIMIT_DMA;
    CHECK(stress_judge(&o) == STRESS_NONE);
    o.s.dma_free = STRESS_LIMIT_DMA - 1;
    CHECK(stress_judge(&o) == STRESS_DMA_FLOOR);
    o = clean(), o.stack_low = STRESS_LIMIT_STACK;
    CHECK(stress_judge(&o) == STRESS_NONE);
    o.stack_low = STRESS_LIMIT_STACK - 1;
    CHECK(stress_judge(&o) == STRESS_STACK);
    // Only a stack that fell during the run counts
    CHECK(stress_stack_fell_low(255, 256));        // started at the floor, now under it
    CHECK(stress_stack_fell_low(200, UINT32_MAX)); // a task that started during the run
    CHECK(!stress_stack_fell_low(200, 200));       // low since boot, not this run's doing
    CHECK(!stress_stack_fell_low(200, 232));       // low at start, fell by the margin only: reported, not failed
    CHECK(stress_stack_fell_low(199, 232));        // fell by more than the margin
    CHECK(!stress_stack_fell_low(256, 900));
    // The DMA floor is not judged while a DMA gate round holds its 8 KB
    o = clean(), o.s.dma_free = STRESS_LIMIT_DMA - 1, o.in_dma_round = true;
    CHECK(stress_judge(&o) == STRESS_NONE);
    // Lockcheck hits since the start, also across a `fw ui lockcheck reset` during the run
    CHECK(stress_since(7, 5) == 2 && stress_since(5, 5) == 0 && stress_since(3, 5) == 3);
    // Two at once: the earlier rule in the order wins
    o = clean(), o.stack_low = 10, o.mismatches = 1;
    CHECK(stress_judge(&o) == STRESS_MISMATCH);
    CHECK(stress_failed(STRESS_STACK) && stress_failed(STRESS_MISMATCH) && stress_failed(STRESS_HUNG));
    CHECK(!stress_failed(STRESS_UPDATE) && !stress_failed(STRESS_HOT) && !stress_failed(STRESS_C6));
    CHECK(strcmp(stress_reason_name(STRESS_HUNG), "hung") == 0 && strcmp(stress_reason_name(STRESS_C6), "c6") == 0);
    CHECK(strcmp(stress_reason_name(STRESS_UPDATE), "update") == 0 && strcmp(stress_reason_name(STRESS_HOT), "hot") == 0);
    CHECK(strcmp(stress_reason_name(STRESS_REASONS), "?") == 0);
    CHECK(!stress_failed(STRESS_NONE) && !stress_failed(STRESS_ESC) && !stress_failed(STRESS_STOP_CMD));
    CHECK(!stress_failed(STRESS_POWEROFF) && !stress_failed(STRESS_UPDATE));
}

static void test_latch(void) {
    stress_run_t run;
    stress_run_reset(&run);
    CHECK(!stress_latch(&run, STRESS_NONE, 5));
    CHECK(stress_latch(&run, STRESS_LATENCY, 42));
    CHECK(!stress_latch(&run, STRESS_HEAP, 50)); // the first failure wins
    CHECK(!stress_latch(&run, STRESS_ESC, 51));
    CHECK(run.reason == STRESS_LATENCY && run.stop_s == 42);
    stress_run_reset(&run);
    CHECK(stress_latch(&run, STRESS_STOP_CMD, 7) && !stress_latch(&run, STRESS_DMA_BAD, 8));
    CHECK(run.reason == STRESS_STOP_CMD);
}

static void test_track(void) {
    stress_run_t run;
    stress_run_reset(&run);
    stress_sample_t a = sample(1000), b = sample(500), c = sample(2000);
    b.pool_peak = 3, b.lat_ms = 40, b.pool_drop = 2;
    c.pool_peak = 12, c.lat_ms = 1;
    stress_track(&run, &a);
    CHECK(run.have && run.lo.dma_free == 1001 && run.lo.pool_peak == 8);
    stress_track(&run, &b);
    stress_track(&run, &c);
    CHECK(run.lo.dma_free == 501 && run.lo.dma_min == 502 && run.lo.dma_largest == 503);
    CHECK(run.lo.int_free == 504 && run.lo.int_min == 505 && run.lo.int_largest == 506 && run.lo.psram_free == 507);
    CHECK(run.lo.pool_peak == 12 && run.lo.lat_ms == 40 && run.lo.pool_drop == 2); // the highest of these
}

static void test_outside(void) {
    stress_outside_t in = {0};
    CHECK(stress_outside(&in) == STRESS_NONE);
    in.c6_trouble = true;
    CHECK(stress_outside(&in) == STRESS_C6);
    in.hot = true;
    CHECK(stress_outside(&in) == STRESS_HOT);
    in.update = true;
    CHECK(stress_outside(&in) == STRESS_UPDATE);
    in.poweroff = true;
    CHECK(stress_outside(&in) == STRESS_POWEROFF);
    in.asked = STRESS_ESC; // a stop that was asked for comes first
    CHECK(stress_outside(&in) == STRESS_ESC);
}

static void test_refusal(void) {
    stress_gate_in_t in = {.dma_free = STRESS_START_DMA, .psram_free = STRESS_START_PSRAM};
    CHECK(stress_refusal(&in) == NULL);
    in.running = true;
    CHECK(strcmp(stress_refusal(&in), "a stress run is already on") == 0);
    in = (stress_gate_in_t){.dma_free = STRESS_START_DMA, .psram_free = STRESS_START_PSRAM, .update = true};
    CHECK(strcmp(stress_refusal(&in), "an update or power off is under way") == 0);
    in = (stress_gate_in_t){.dma_free = STRESS_START_DMA, .psram_free = STRESS_START_PSRAM, .c6_trouble = true};
    CHECK(strcmp(stress_refusal(&in), "the C6 link is not up") == 0);
    in = (stress_gate_in_t){.dma_free = STRESS_START_DMA, .psram_free = STRESS_START_PSRAM, .hot = true};
    CHECK(strcmp(stress_refusal(&in), "the badge is hot (thermal warning)") == 0);
    in = (stress_gate_in_t){.dma_free = STRESS_START_DMA - 1, .psram_free = STRESS_START_PSRAM};
    CHECK(strcmp(stress_refusal(&in), "low memory") == 0);
    in = (stress_gate_in_t){.dma_free = STRESS_START_DMA, .psram_free = STRESS_START_PSRAM - 1};
    CHECK(strcmp(stress_refusal(&in), "low memory") == 0);
}

static void test_lines(void) {
    char            line[STRESS_LINE_MAX];
    stress_sample_t s = {98304, 85000, 65536, 150000, 140000, 110592, 27883616, 7, 0, 4};
    stress_fmt_start(line, sizeof(line), 10, true, true);
    CHECK(strcmp(line, "start 10 min workers dma,sd,fs,ui,lat") == 0);
    stress_fmt_start(line, sizeof(line), 1, false, true);
    CHECK(strcmp(line, "start 1 min workers dma,fs,ui,lat sd skipped: no card") == 0);
    stress_fmt_start(line, sizeof(line), 30, true, false);
    CHECK(strcmp(line, "start 30 min workers dma,sd,ui,lat fs skipped: no drive") == 0);
    stress_fmt_sample(line, sizeof(line), 10, &s, STRESS_NONE);
    CHECK(strcmp(line, "t=10s dma free=98304 min=85000 largest=65536 int free=150000 min=140000 largest=110592 "
                       "psram=27883616 pool peak=7 drop=0 lat=4ms ok") == 0);
    stress_fmt_sample(line, sizeof(line), 40, &s, STRESS_LATENCY);
    CHECK(strstr(line, "t=40s dma free=98304 ") == line && strcmp(line + strlen(line) - 15, "lat=4ms latency") == 0);
    stress_fmt_min(line, sizeof(line), &s);
    CHECK(strcmp(line, "min dma free=98304 min=85000 largest=65536 int free=150000 min=140000 largest=110592 "
                       "psram=27883616 pool peak=7 drop=0 lat=4ms") == 0);
    stress_fmt_stop(line, sizeof(line), STRESS_LATENCY, 42);
    CHECK(strcmp(line, "STOP latency at 42s") == 0);
    stress_fmt_stop(line, sizeof(line), STRESS_ESC, 7);
    CHECK(strcmp(line, "STOP esc at 7s") == 0);
    stress_fmt_done(line, sizeof(line), 600, true);
    CHECK(strcmp(line, "done 600s PASS") == 0);
    stress_fmt_done(line, sizeof(line), 42, false);
    CHECK(strcmp(line, "done 42s FAIL") == 0);
    stress_fmt_stack(line, sizeof(line), "stress-mon", 812, 4096);
    CHECK(strcmp(line, "stack stress-mon 812/4096") == 0);
    stress_fmt_stack(line, sizeof(line), "ipc0", 500, 0);
    CHECK(strcmp(line, "stack ipc0 500/?") == 0);
    stress_counts_t c = {{[STRESS_C_DMA] = 30, [STRESS_C_DMA_BUSY] = 1, [STRESS_C_DMA_BAD] = 2,
                          [STRESS_C_COPIES] = 30, [STRESS_C_MISMATCH] = 3, [STRESS_C_ALLOC] = 4, [STRESS_C_HEAP] = 5,
                          [STRESS_C_LOCK] = 6, [STRESS_C_SD] = 100, [STRESS_C_SD_KB] = 6400, [STRESS_C_SD_IO] = 7,
                          [STRESS_C_FS] = 60, [STRESS_C_FS_IO] = 8, [STRESS_C_FRAMES] = 6000}};
    stress_fmt_counts(line, sizeof(line), &c, false);
    CHECK(strcmp(line, "errors dma_bad=2 dma_busy=1 mismatch=3 alloc=4 heap=5 lock=6 sd_io=7 fs_io=8") == 0);
    CHECK(stress_errors(&c) == 2 + 3 + 4 + 5 + 6 + 7 + 8); // a busy gate is a skip, not an error
    stress_fmt_counts(line, sizeof(line), &c, true);
    CHECK(strcmp(line, "work dma=30 copies=30 sd=100 sd_kb=6400 fs=60 frames=6000") == 0);
    char cut[10];
    CHECK(stress_fmt_counts(cut, sizeof(cut), &c, true) == strlen(line) && strcmp(cut, "work dma=") == 0);
    // One physical line each
    CHECK(strchr(line, '\n') == NULL);
}

// Every figure at its widest still fits STRESS_LINE_MAX; a short buffer is cut, terminated, and told the whole length
static void test_truncation(void) {
    char            line[STRESS_LINE_MAX];
    // Internal RAM is under 1 MB, PSRAM 32 MB, a pool's peak 16 bits; drops only grow, a stall is under 100 s
    stress_sample_t w = {999999, 999999, 999999, 999999, 999999, 999999, 33554432, UINT16_MAX, UINT32_MAX, 99999};
    size_t          n = stress_fmt_sample(line, sizeof(line), 1800, &w, STRESS_DMA_FLOOR);
    CHECK(n < sizeof(line) && strlen(line) == n);
    CHECK(strcmp(line + n - 9, "dma_floor") == 0);
    n = stress_fmt_min(line, sizeof(line), &w);
    CHECK(n < sizeof(line) && strlen(line) == n);
    // Past those, the line is cut, terminated, and the whole length is still returned
    stress_sample_t huge = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                            UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    n = stress_fmt_sample(line, sizeof(line), 1800, &huge, STRESS_NONE);
    CHECK(n >= sizeof(line) && strlen(line) == sizeof(line) - 1);
    stress_counts_t c;
    memset(&c, 0xff, sizeof(c));
    CHECK(stress_fmt_counts(line, sizeof(line), &c, false) < sizeof(line));
    CHECK(stress_fmt_counts(line, sizeof(line), &c, true) < sizeof(line));
    CHECK(stress_fmt_start(line, sizeof(line), 30, false, false) < sizeof(line));
    // A task name past FreeRTOS's 15 characters is cut there
    stress_fmt_stack(line, sizeof(line), "a-very-long-task-name", 1, 2);
    CHECK(strcmp(line, "stack a-very-long-tas 1/2") == 0);
    char small[12];
    memset(small, 'x', sizeof(small));
    stress_sample_t s = {98304, 85000, 65536, 150000, 140000, 110592, 27883616, 7, 0, 4};
    n = stress_fmt_sample(small, sizeof(small), 10, &s, STRESS_NONE);
    CHECK(n > sizeof(small) && strlen(small) == sizeof(small) - 1 && strcmp(small, "t=10s dma f") == 0);
    n = stress_fmt_done(small, sizeof(small), 600, true);
    CHECK(n == 14 && strcmp(small, "done 600s P") == 0);
    CHECK(stress_fmt_stop(NULL, 0, STRESS_HEAP, 3) == strlen("STOP heap at 3s"));
}

static void test_screen(void) {
    char line[200];
    stress_fmt_clock(line, sizeof(line), 200, 600);
    CHECK(strcmp(line, "03:20 / 10:00") == 0);
    stress_fmt_clock(line, sizeof(line), 5, 1800);
    CHECK(strcmp(line, "00:05 / 30:00") == 0);
    stress_counts_t c = {{[STRESS_C_DMA] = 12, [STRESS_C_SD] = 340, [STRESS_C_FS] = 3, [STRESS_C_FRAMES] = 600}};
    stress_fmt_workers(line, sizeof(line), &c, 0, 0, 4);
    CHECK(strcmp(line, "DMA: ok, 12 rounds\nSD: ok, 340 files\nStorage: ok, 3 files\nUI: ok, 600 frames\n"
                       "Latency: ok, worst 4 ms") == 0);
    stress_fmt_workers(line, sizeof(line), &c, STRESS_W_FS | STRESS_W_UI | STRESS_W_LAT, STRESS_W_SD, 300);
    CHECK(strcmp(line, "DMA: ok, 12 rounds\nSD: skipped, no card\nStorage: FAIL, 3 files\nUI: FAIL, 600 frames\n"
                       "Latency: FAIL, worst 300 ms") == 0);
    stress_fmt_workers(line, sizeof(line), &c, STRESS_W_DMA | STRESS_W_SD, STRESS_W_FS, 1);
    CHECK(strstr(line, "DMA: FAIL, 12 rounds\nSD: FAIL, 340 files\nStorage: skipped, no drive\n") == line);
    char small[8];
    size_t n = stress_fmt_workers(small, sizeof(small), &c, 0, 0, 4);
    CHECK(n > sizeof(small) && strcmp(small, "DMA: ok") == 0);
}

int main(void) {
    test_screen();
    test_parse();
    test_rules_alone();
    test_latch();
    test_track();
    test_outside();
    test_refusal();
    test_lines();
    test_truncation();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("stress_calc_test ok\n");
    return 0;
}
