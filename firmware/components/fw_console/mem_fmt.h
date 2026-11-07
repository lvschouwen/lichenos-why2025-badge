#pragma once

#include <stddef.h>
#include <stdint.h>

// The "fw mem" heap line with the totals and percentages the standard's limits are written in (#205, docs/code-standard.md).
// Pure maths and formatting, host-tested in host_tests/mem_fmt_test.c. The format is stable: the MVP bench gate greps
// "min_pct" of the internal line.

// "mem internal free 123 largest 100 min 80 total 400 free_pct 30 min_pct 20 largest_pct 81 used_pct 69"
// free_pct/min_pct/used_pct are of total; largest_pct is of free (fragmentation view). A heap of total 0 prints 0 for all.
size_t mem_fmt_heap(char *out, size_t cap, char const *name, uint32_t free_b, uint32_t largest, uint32_t min_free,
                    uint32_t total);
