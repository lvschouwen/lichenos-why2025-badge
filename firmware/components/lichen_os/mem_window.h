#pragma once

// A low-water mark per heap that can be restarted (#410). The heap's own minimum counts since boot and cannot be
// reset without changing what every other reader of it sees (IDF's local-minimum monitor replaces it until stopped),
// so a load test early in a run hides any slow drain after it. This window is kept beside it, from samples:
// - every sample folds the heap's free figure into the window minimum;
// - when the since-boot minimum is below where it stood at the reset, the heap reached it after the reset, so it is
//   folded in too: the window minimum is then the true lowest point, however short the dip ("exact 1");
// - otherwise ("exact 0") a dip above the since-boot low that is shorter than the sampling period can be missed.
// The age is uptime ms in 32 bits: it wraps after 49.7 days, and an age is right across one wrap. Pure (no ESP-IDF),
// host-tested in host_tests/mem_window_test.c.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MEM_WINDOW_SAMPLE_MS 250 // dma_watch.c's tick, which samples the window (asserted there)

typedef enum {
    MEM_WINDOW_INTERNAL, // MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, as fw mem's internal line
    MEM_WINDOW_DMA,      // MALLOC_CAP_DMA
    MEM_WINDOW_PSRAM,    // MALLOC_CAP_SPIRAM
    MEM_WINDOW_HEAPS,
} mem_window_heap_t;

typedef struct {
    uint32_t free_now; // the heap's free bytes now
    uint32_t boot_min; // the heap's minimum free since boot (the figure fw mem prints as min)
} mem_window_fig_t;

typedef struct {
    uint32_t min[MEM_WINDOW_HEAPS];      // the window minimum
    uint32_t boot_ref[MEM_WINDOW_HEAPS]; // the since-boot minimum at the reset
    uint32_t boot_now[MEM_WINDOW_HEAPS]; // the since-boot minimum at the last sample, for the line
    uint32_t start_ms;                   // uptime of the reset
    uint32_t resets;                     // resets after the first (the sampler's start)
    uint32_t gen;                        // bumped by every reset: a sample read before one is dropped
    bool     started;
} mem_window_t;

// The generation to read before the figures of a sample, and to pass to mem_window_sample with them
static inline uint32_t mem_window_gen(mem_window_t const *w) {
    return w->gen;
}

// Restarts the window at now_ms from the figures now. The first call starts it; later ones count as resets.
void mem_window_reset(mem_window_t *w, mem_window_fig_t const fig[MEM_WINDOW_HEAPS], uint32_t now_ms);

// Folds one sample in, its figures read at generation gen. False (and nothing stored) when a reset came in between:
// the figures may be from before the reset. Before the first reset it starts the window instead (at now_ms).
bool mem_window_sample(mem_window_t *w, mem_window_fig_t const fig[MEM_WINDOW_HEAPS], uint32_t now_ms, uint32_t gen);

// The line for one heap at now_ms; 0 when the window has not started or heap is out of range.
// "mem window internal min 81234 age_ms 3600250 boot_min 70000 resets 1 exact 0 sample_ms 250"
size_t mem_window_line(mem_window_t const *w, unsigned heap, uint32_t now_ms, char *out, size_t cap);
