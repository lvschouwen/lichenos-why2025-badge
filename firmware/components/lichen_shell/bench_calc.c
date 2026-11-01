#include "bench_calc.h"

#include <stdio.h>

static size_t len_of(int n) {
    return n < 0 ? 0 : (size_t)n;
}

unsigned bench_rate_x10(uint64_t bytes, uint64_t us) {
    if (us == 0) {
        return 0;
    }
    // bytes / us * 1e6 / 1 MB, in tenths, rounded
    return (unsigned)((bytes * 10000000ull + (us * BENCH_MB) / 2) / (us * BENCH_MB));
}

size_t bench_fmt_rate(char *out, size_t cap, uint64_t bytes, uint64_t us) {
    if (us == 0) {
        return len_of(snprintf(out, cap, "-"));
    }
    unsigned r = bench_rate_x10(bytes, us);
    return len_of(snprintf(out, cap, "%u.%u MB/s", r / 10, r % 10));
}

size_t bench_fmt_redraw(char *out, size_t cap, uint64_t us_total, unsigned frames, uint64_t flush_us, unsigned strips) {
    if (frames == 0) {
        return len_of(snprintf(out, cap, "-"));
    }
    unsigned per10 = (unsigned)((us_total / frames + 50) / 100);
    if (strips == 0) {
        return len_of(snprintf(out, cap, "%u.%u ms/frame", per10 / 10, per10 % 10));
    }
    unsigned fl10 = (unsigned)((flush_us / strips + 50) / 100);
    return len_of(snprintf(out, cap, "%u.%u ms/frame, flush %u.%u ms/strip", per10 / 10, per10 % 10, fl10 / 10, fl10 % 10));
}

size_t bench_fmt_scroll(char *out, size_t cap, unsigned frames, uint32_t span_ms, uint32_t render_avg_us) {
    if (frames == 0 || span_ms == 0) {
        return len_of(snprintf(out, cap, "-"));
    }
    unsigned fps10 = (unsigned)(((uint64_t)frames * 10000 + span_ms / 2) / span_ms);
    return len_of(snprintf(out, cap, "%u.%u fps, render %u ms", fps10 / 10, fps10 % 10, (unsigned)((render_avg_us + 500) / 1000)));
}

size_t bench_fmt_mem(char *out, size_t cap, size_t int_free, size_t int_min, size_t dma_free, size_t psram_free) {
    unsigned ps10 = (unsigned)((psram_free * 10 + BENCH_MB / 2) / BENCH_MB);
    return len_of(snprintf(out, cap, "int %u KB (min %u), DMA %u KB, PSRAM %u.%u MB", (unsigned)(int_free / 1024),
                           (unsigned)(int_min / 1024), (unsigned)(dma_free / 1024), ps10 / 10, ps10 % 10));
}
