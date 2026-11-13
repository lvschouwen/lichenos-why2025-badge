#include "mem_window.h"

#include <stdio.h>

static char const *const NAMES[MEM_WINDOW_HEAPS] = {"internal", "dma", "psram"};

void mem_window_reset(mem_window_t *w, mem_window_fig_t const fig[MEM_WINDOW_HEAPS], uint32_t now_ms) {
    for (int i = 0; i < MEM_WINDOW_HEAPS; i++) {
        w->min[i]      = fig[i].free_now;
        w->boot_ref[i] = fig[i].boot_min;
        w->boot_now[i] = fig[i].boot_min;
    }
    if (w->started) {
        w->resets++;
    }
    w->gen++;
    w->start_ms = now_ms;
    w->started  = true;
}

bool mem_window_sample(mem_window_t *w, mem_window_fig_t const fig[MEM_WINDOW_HEAPS], uint32_t now_ms, uint32_t gen) {
    if (gen != w->gen) {
        return false; // a reset came between reading the figures and storing them: a pre-reset low would leak in
    }
    if (!w->started) {
        mem_window_reset(w, fig, now_ms);
        return true;
    }
    for (int i = 0; i < MEM_WINDOW_HEAPS; i++) {
        if (fig[i].free_now < w->min[i]) {
            w->min[i] = fig[i].free_now;
        }
        // A since-boot low below the one at the reset was reached after it, between two samples or not
        if (fig[i].boot_min < w->boot_ref[i] && fig[i].boot_min < w->min[i]) {
            w->min[i] = fig[i].boot_min;
        }
        w->boot_now[i] = fig[i].boot_min;
    }
    return true;
}

size_t mem_window_line(mem_window_t const *w, unsigned heap, uint32_t now_ms, char *out, size_t cap) {
    if (!w->started || heap >= MEM_WINDOW_HEAPS || cap == 0) {
        return 0;
    }
    uint32_t age = now_ms - w->start_ms; // unsigned: right across one wrap of the 32-bit ms
    // exact: the since-boot low moved since the reset, so the window minimum is the true lowest point
    bool exact = w->boot_now[heap] < w->boot_ref[heap];
    int  n = snprintf(out, cap, "mem window %s min %u age_ms %u boot_min %u resets %u exact %u sample_ms %u", NAMES[heap],
                      (unsigned)w->min[heap], (unsigned)age, (unsigned)w->boot_now[heap], (unsigned)w->resets,
                      exact ? 1u : 0u, (unsigned)MEM_WINDOW_SAMPLE_MS);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n < cap ? (size_t)n : cap - 1;
}
