// Host test: the resettable low-water mark per heap behind "fw mem window" (#410)
#include "mem_window.h"

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

static char line[160];

static char const *at(mem_window_t const *w, unsigned heap, uint32_t now_ms) {
    return mem_window_line(w, heap, now_ms, line, sizeof(line)) ? line : "";
}

// internal, dma, psram: the same figures for the three heaps, scaled so a mix-up shows
static void figs(mem_window_fig_t f[MEM_WINDOW_HEAPS], uint32_t free_now, uint32_t boot_min) {
    for (int i = 0; i < MEM_WINDOW_HEAPS; i++) {
        f[i].free_now = free_now * (uint32_t)(i + 1);
        f[i].boot_min = boot_min * (uint32_t)(i + 1);
    }
}

static void not_started(void) {
    mem_window_t w = {0};
    CHECK(mem_window_line(&w, MEM_WINDOW_INTERNAL, 100, line, sizeof(line)) == 0);
    mem_window_fig_t f[MEM_WINDOW_HEAPS];
    figs(f, 1000, 900);
    mem_window_sample(&w, f, 100); // the first sample starts it, as no reset
    CHECK(mem_window_line(&w, MEM_WINDOW_HEAPS, 100, line, sizeof(line)) == 0);
    CHECK(strcmp(at(&w, MEM_WINDOW_INTERNAL, 100), "mem window internal min 1000 age_ms 0 boot_min 900 resets 0") == 0);
}

static void tracks_the_minimum(void) {
    mem_window_t     w = {0};
    mem_window_fig_t f[MEM_WINDOW_HEAPS];
    figs(f, 1000, 900);
    mem_window_reset(&w, f, 0);
    uint32_t const seq[] = {1000, 980, 990, 950, 970, 1000};
    for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        figs(f, seq[i], 900); // above the since-boot low: only the samples count
        mem_window_sample(&w, f, 250 * (i + 1));
    }
    CHECK(strcmp(at(&w, MEM_WINDOW_INTERNAL, 1500), "mem window internal min 950 age_ms 1500 boot_min 900 resets 0") == 0);
    CHECK(strcmp(at(&w, MEM_WINDOW_DMA, 1500), "mem window dma min 1900 age_ms 1500 boot_min 1800 resets 0") == 0);
    CHECK(strcmp(at(&w, MEM_WINDOW_PSRAM, 1500), "mem window psram min 2850 age_ms 1500 boot_min 2700 resets 0") == 0);

    // A dip between two samples to a new since-boot low: the samples never saw it, the heap's mark did
    figs(f, 990, 800);
    mem_window_sample(&w, f, 1750);
    CHECK(strcmp(at(&w, MEM_WINDOW_INTERNAL, 1750), "mem window internal min 800 age_ms 1750 boot_min 800 resets 0") == 0);
}

static void reset_keeps_the_boot_low(void) {
    mem_window_t     w = {0};
    mem_window_fig_t f[MEM_WINDOW_HEAPS];
    figs(f, 1000, 1000);
    mem_window_reset(&w, f, 0);
    figs(f, 500, 500); // the load test pulls everything down
    mem_window_sample(&w, f, 1000);
    figs(f, 1000, 500); // and gives it back
    mem_window_sample(&w, f, 2000);
    CHECK(strcmp(at(&w, MEM_WINDOW_INTERNAL, 2000), "mem window internal min 500 age_ms 2000 boot_min 500 resets 0") == 0);

    mem_window_reset(&w, f, 3000);
    CHECK(strcmp(at(&w, MEM_WINDOW_INTERNAL, 3000), "mem window internal min 1000 age_ms 0 boot_min 500 resets 1") == 0);
    // A slow drain after it shows in the window; the since-boot low stays the load test's
    figs(f, 900, 500);
    mem_window_sample(&w, f, 4000);
    figs(f, 800, 500);
    mem_window_sample(&w, f, 5000);
    CHECK(strcmp(at(&w, MEM_WINDOW_INTERNAL, 5000), "mem window internal min 800 age_ms 2000 boot_min 500 resets 1") == 0);
    // A since-boot low that did not move since the reset is not folded in
    CHECK(w.min[MEM_WINDOW_INTERNAL] == 800);
    mem_window_reset(&w, f, 6000);
    CHECK(strstr(at(&w, MEM_WINDOW_INTERNAL, 6000), " resets 2") != NULL);
}

static void age_wraps(void) {
    mem_window_t     w = {0};
    mem_window_fig_t f[MEM_WINDOW_HEAPS];
    figs(f, 1000, 900);
    mem_window_reset(&w, f, 0xFFFFFF00u); // 49.7 days of uptime, just before the 32-bit ms wrap
    mem_window_sample(&w, f, 0x00000100u);
    CHECK(strcmp(at(&w, MEM_WINDOW_INTERNAL, 0x00000100u),
                 "mem window internal min 1000 age_ms 512 boot_min 900 resets 0") == 0);
}

static void format_and_cap(void) {
    mem_window_t     w = {0};
    mem_window_fig_t f[MEM_WINDOW_HEAPS];
    figs(f, UINT32_MAX / 3, UINT32_MAX / 3);
    mem_window_reset(&w, f, 0);
    size_t n = mem_window_line(&w, MEM_WINDOW_PSRAM, UINT32_MAX, line, sizeof(line));
    CHECK(n == strlen(line));
    CHECK(strcmp(line, "mem window psram min 4294967295 age_ms 4294967295 boot_min 4294967295 resets 0") == 0);
    char small[12];
    n = mem_window_line(&w, MEM_WINDOW_INTERNAL, 0, small, sizeof(small));
    CHECK(n == sizeof(small) - 1 && strlen(small) == n);
    CHECK(strcmp(small, "mem window ") == 0);
}

int main(void) {
    not_started();
    tracks_the_minimum();
    reset_keeps_the_boot_low();
    age_wraps();
    format_and_cap();
    if (failures) {
        printf("mem_window_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("mem_window_test: all passed\n");
    return 0;
}
