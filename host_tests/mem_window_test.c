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

// A sample read and stored with no reset in between
static void sample(mem_window_t *w, mem_window_fig_t const f[MEM_WINDOW_HEAPS], uint32_t now_ms) {
    CHECK(mem_window_sample(w, f, now_ms, mem_window_gen(w)));
}

static void not_started(void) {
    mem_window_t w = {0};
    CHECK(mem_window_line(&w, MEM_WINDOW_INTERNAL, 100, line, sizeof(line)) == 0);
    mem_window_fig_t f[MEM_WINDOW_HEAPS];
    figs(f, 1000, 900);
    sample(&w, f, 100); // the first sample starts it, as no reset
    CHECK(mem_window_line(&w, MEM_WINDOW_HEAPS, 100, line, sizeof(line)) == 0);
    CHECK(strcmp(at(&w, MEM_WINDOW_INTERNAL, 100), "mem window internal min 1000 age_ms 0 boot_min 900 resets 0 exact 0 sample_ms 250") == 0);
}

static void tracks_the_minimum(void) {
    mem_window_t     w = {0};
    mem_window_fig_t f[MEM_WINDOW_HEAPS];
    figs(f, 1000, 900);
    mem_window_reset(&w, f, 0);
    uint32_t const seq[] = {1000, 980, 990, 950, 970, 1000};
    for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        figs(f, seq[i], 900); // above the since-boot low: only the samples count
        sample(&w, f, 250 * (i + 1));
    }
    CHECK(strcmp(at(&w, MEM_WINDOW_INTERNAL, 1500), "mem window internal min 950 age_ms 1500 boot_min 900 resets 0 exact 0 sample_ms 250") == 0);
    CHECK(strcmp(at(&w, MEM_WINDOW_DMA, 1500), "mem window dma min 1900 age_ms 1500 boot_min 1800 resets 0 exact 0 sample_ms 250") == 0);
    CHECK(strcmp(at(&w, MEM_WINDOW_PSRAM, 1500), "mem window psram min 2850 age_ms 1500 boot_min 2700 resets 0 exact 0 sample_ms 250") == 0);

    // A dip between two samples to a new since-boot low: the samples never saw it, the heap's mark did
    figs(f, 990, 800);
    sample(&w, f, 1750);
    CHECK(strcmp(at(&w, MEM_WINDOW_INTERNAL, 1750), "mem window internal min 800 age_ms 1750 boot_min 800 resets 0 exact 1 sample_ms 250") == 0);
}

static void reset_keeps_the_boot_low(void) {
    mem_window_t     w = {0};
    mem_window_fig_t f[MEM_WINDOW_HEAPS];
    figs(f, 1000, 1000);
    mem_window_reset(&w, f, 0);
    figs(f, 500, 500); // the load test pulls everything down
    sample(&w, f, 1000);
    figs(f, 1000, 500); // and gives it back
    sample(&w, f, 2000);
    CHECK(strcmp(at(&w, MEM_WINDOW_INTERNAL, 2000), "mem window internal min 500 age_ms 2000 boot_min 500 resets 0 exact 1 sample_ms 250") == 0);

    mem_window_reset(&w, f, 3000);
    CHECK(strcmp(at(&w, MEM_WINDOW_INTERNAL, 3000), "mem window internal min 1000 age_ms 0 boot_min 500 resets 1 exact 0 sample_ms 250") == 0);
    // A slow drain after it shows in the window; the since-boot low stays the load test's
    figs(f, 900, 500);
    sample(&w, f, 4000);
    figs(f, 800, 500);
    sample(&w, f, 5000);
    CHECK(strcmp(at(&w, MEM_WINDOW_INTERNAL, 5000), "mem window internal min 800 age_ms 2000 boot_min 500 resets 1 exact 0 sample_ms 250") == 0);
    // A since-boot low that did not move since the reset is not folded in
    CHECK(w.min[MEM_WINDOW_INTERNAL] == 800);
    mem_window_reset(&w, f, 6000);
    CHECK(strstr(at(&w, MEM_WINDOW_INTERNAL, 6000), " resets 2") != NULL);
}

// A tick reads the generation, then the figures; a reset lands before it stores them: its pre-reset low is dropped
static void stale_sample_dropped(void) {
    mem_window_t     w = {0};
    mem_window_fig_t f[MEM_WINDOW_HEAPS];
    figs(f, 1000, 900);
    mem_window_reset(&w, f, 0);
    uint32_t         gen = mem_window_gen(&w);
    mem_window_fig_t stale[MEM_WINDOW_HEAPS];
    figs(stale, 600, 900); // the load test's low, read before the reset
    figs(f, 1000, 900);
    mem_window_reset(&w, f, 100);
    CHECK(!mem_window_sample(&w, stale, 150, gen));
    CHECK(w.min[MEM_WINDOW_INTERNAL] == 1000 && w.min[MEM_WINDOW_PSRAM] == 3000);
    CHECK(mem_window_sample(&w, stale, 200, mem_window_gen(&w))); // the same figures read after it do count
    CHECK(w.min[MEM_WINDOW_INTERNAL] == 600);
}

static void age_wraps(void) {
    mem_window_t     w = {0};
    mem_window_fig_t f[MEM_WINDOW_HEAPS];
    figs(f, 1000, 900);
    mem_window_reset(&w, f, 0xFFFFFF00u); // 49.7 days of uptime, just before the 32-bit ms wrap
    sample(&w, f, 0x00000100u);
    CHECK(strcmp(at(&w, MEM_WINDOW_INTERNAL, 0x00000100u),
                 "mem window internal min 1000 age_ms 512 boot_min 900 resets 0 exact 0 sample_ms 250") == 0);
}

static void format_and_cap(void) {
    mem_window_t     w = {0};
    mem_window_fig_t f[MEM_WINDOW_HEAPS];
    figs(f, UINT32_MAX / 3, UINT32_MAX / 3);
    mem_window_reset(&w, f, 0);
    size_t n = mem_window_line(&w, MEM_WINDOW_PSRAM, UINT32_MAX, line, sizeof(line));
    CHECK(n == strlen(line));
    CHECK(strcmp(line, "mem window psram min 4294967295 age_ms 4294967295 boot_min 4294967295 resets 0 exact 0 sample_ms 250") == 0);
    char small[12];
    n = mem_window_line(&w, MEM_WINDOW_INTERNAL, 0, small, sizeof(small));
    CHECK(n == sizeof(small) - 1 && strlen(small) == n);
    CHECK(strcmp(small, "mem window ") == 0);
}

// The heaps apart (#410 review): only DMA reaches a new since-boot low after the reset, so only its window is exact;
// internal dips above its since-boot low and PSRAM stays put (its window is still its free figure at the reset)
static void heaps_apart(void) {
    mem_window_t     w = {0};
    mem_window_fig_t f[MEM_WINDOW_HEAPS] = {{1000, 900}, {5000, 4000}, {30000, 29000}};
    mem_window_reset(&w, f, 0);
    f[MEM_WINDOW_INTERNAL] = (mem_window_fig_t){950, 900};   // above its boot low: not exact
    f[MEM_WINDOW_DMA]      = (mem_window_fig_t){3900, 3800}; // a new boot low after the reset: exact
    sample(&w, f, 250);
    f[MEM_WINDOW_INTERNAL] = (mem_window_fig_t){990, 900};
    f[MEM_WINDOW_DMA]      = (mem_window_fig_t){4500, 3800};
    sample(&w, f, 500);
    CHECK(strcmp(at(&w, MEM_WINDOW_INTERNAL, 500), "mem window internal min 950 age_ms 500 boot_min 900 resets 0 exact 0 sample_ms 250") == 0);
    CHECK(strcmp(at(&w, MEM_WINDOW_DMA, 500), "mem window dma min 3800 age_ms 500 boot_min 3800 resets 0 exact 1 sample_ms 250") == 0);
    CHECK(strcmp(at(&w, MEM_WINDOW_PSRAM, 500), "mem window psram min 30000 age_ms 500 boot_min 29000 resets 0 exact 0 sample_ms 250") == 0);
}

int main(void) {
    not_started();
    tracks_the_minimum();
    reset_keeps_the_boot_low();
    stale_sample_dropped();
    age_wraps();
    format_and_cap();
    heaps_apart();
    if (failures) {
        printf("mem_window_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("mem_window_test: all passed\n");
    return 0;
}
