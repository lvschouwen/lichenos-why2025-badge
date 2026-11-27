// PMP and PMA entry text (#608). See pmp_fmt.h.
#include "pmp_fmt.h"

#include <stdio.h>

enum { MODE_OFF, MODE_TOR, MODE_NA4, MODE_NAPOT };

static char const *const MODE_NAME[] = {"OFF", "TOR", "NA4", "NAPOT"};

// The byte range [*lo, *hi] an entry matches. Both encodings store address >> 2; NAPOT's trailing ones give the size.
static void range_of(int mode, uint32_t addr, uint32_t prev_addr, uint64_t *lo, uint64_t *hi) {
    if (mode == MODE_TOR) {
        *lo = (uint64_t)prev_addr << 2;
        *hi = ((uint64_t)addr << 2) - 1; // an empty TOR (addr <= prev) shows hi < lo
        return;
    }
    if (mode == MODE_NA4) {
        *lo = (uint64_t)addr << 2;
        *hi = *lo + 3;
        return;
    }
    unsigned ones = 0;
    while (ones < 32 && (addr >> ones) & 1u) {
        ones++;
    }
    // All 32 bits set is the whole 34-bit space (2^34), not 8 << 32: the field has no bit left for a size bit
    uint64_t const size = ones == 32 ? 1ull << 34 : 8ull << ones;
    *lo                 = ((uint64_t)addr << 2) & ~(size - 1);
    *hi                 = *lo + size - 1;
}

static size_t clamp(int n, size_t cap) {
    if (n < 0 || cap == 0) {
        return 0;
    }
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

// Appends " 0x<lo>-0x<hi> (<size>)" for a mode that matches something.
static size_t put_range(char *out, size_t cap, int mode, uint32_t addr, uint32_t prev_addr) {
    uint64_t lo, hi;
    range_of(mode, addr, prev_addr, &lo, &hi);
    if (hi < lo) {
        return clamp(snprintf(out, cap, " 0x%08llx-0x%08llx (empty)", (unsigned long long)lo, (unsigned long long)hi), cap);
    }
    uint64_t const size = hi - lo + 1;
    char const    *unit = "B";
    uint64_t       n    = size;
    if (size % (1024ull * 1024 * 1024) == 0) {
        n = size >> 30, unit = "GB";
    } else if (size % (1024ull * 1024) == 0) {
        n = size >> 20, unit = "MB";
    } else if (size % 1024 == 0) {
        n = size >> 10, unit = "KB";
    }
    return clamp(snprintf(out, cap, " 0x%08llx-0x%08llx (%llu %s)", (unsigned long long)lo, (unsigned long long)hi,
                          (unsigned long long)n, unit),
                 cap);
}

size_t pmp_entry_format(char *out, size_t cap, unsigned idx, uint8_t cfg, uint32_t addr, uint32_t prev_addr) {
    int const mode = (cfg >> 3) & 3;
    if (mode == MODE_OFF && !(cfg & 0x80)) {
        return clamp(snprintf(out, cap, "pmp%u off", idx), cap);
    }
    size_t n = clamp(snprintf(out, cap, "pmp%u %c %s %c%c%c", idx, cfg & 0x80 ? 'L' : '-', MODE_NAME[mode],
                              cfg & 1 ? 'r' : '-', cfg & 2 ? 'w' : '-', cfg & 4 ? 'x' : '-'),
                     cap);
    if (mode != MODE_OFF && n + 1 < cap) {
        n += put_range(out + n, cap - n, mode, addr, prev_addr);
    }
    return n;
}

size_t pma_entry_format(char *out, size_t cap, unsigned idx, uint32_t cfg, uint32_t addr, uint32_t prev_addr) {
    int const mode = (int)(cfg >> 30);
    char      attr[24];
    snprintf(attr, sizeof attr, "%s%s%s%s", cfg & (1u << 27) ? " nc" : "", cfg & (1u << 26) ? " wt" : "",
             cfg & (1u << 25) ? " wmna" : "", cfg & (1u << 24) ? " rmna" : "");
    if (!(cfg & 1u)) {
        return clamp(snprintf(out, cap, "pma%u off%s", idx, attr), cap);
    }
    size_t n = clamp(snprintf(out, cap, "pma%u %c %s %c%c%c%s", idx, cfg & (1u << 29) ? 'L' : '-', MODE_NAME[mode],
                              cfg & (1u << 4) ? 'r' : '-', cfg & (1u << 3) ? 'w' : '-', cfg & (1u << 2) ? 'x' : '-',
                              attr),
                     cap);
    if (mode != MODE_OFF && n + 1 < cap) {
        n += put_range(out + n, cap - n, mode, addr, prev_addr);
    }
    return n;
}
