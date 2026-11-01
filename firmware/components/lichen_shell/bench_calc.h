#pragma once

#include <stddef.h>
#include <stdint.h>

// The Bench app's arithmetic and result lines (#96). Pure, host-tested in host_tests/bench_calc_test.c; app_bench.c
// measures, these turn bytes and microseconds into text.

#define BENCH_MB (1024u * 1024u)

// bytes moved in us, as MB/s in tenths (1 MB = 1048576 bytes); 0 when us is 0
unsigned bench_rate_x10(uint64_t bytes, uint64_t us);

// "312.5 MB/s", or "-" when us is 0. Returns the length.
size_t bench_fmt_rate(char *out, size_t cap, uint64_t bytes, uint64_t us);

// "93.1 ms/frame, flush 5.9 ms/strip" (flush part left out when strips is 0), or "-" when frames is 0
size_t bench_fmt_redraw(char *out, size_t cap, uint64_t us_total, unsigned frames, uint64_t flush_us, unsigned strips);

// "10.0 fps, render 79 ms" over span_ms, or "-" when frames or span_ms is 0
size_t bench_fmt_scroll(char *out, size_t cap, unsigned frames, uint32_t span_ms, uint32_t render_avg_us);

// "int 155 KB (min 146), DMA 116 KB, PSRAM 27.9 MB"
size_t bench_fmt_mem(char *out, size_t cap, size_t int_free, size_t int_min, size_t dma_free, size_t psram_free);
