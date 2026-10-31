#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Learned bad PSRAM (#86): the set of 4 KB pages that ever held a bad byte (any detector, any boot) kept in NVS, so
// the quarantine is the union of those pages plus a margin, as separate ranges (an isolated outlier costs one margin
// pair, not the span between it and the cluster). lo/hi (lowest/highest bad byte) are kept as statistics only: they
// are real bad bytes, so their pages seed the set (this is the migration from the span-only records). Pure logic (merge, plausibility, persistence decision), host-tested in host_tests/psram_learn_test.c;
// psram_learned.c holds it in NVS.

#define PSRAM_LEARN_VERSION  2
#define PSRAM_ADDR_MIN       0x48000000u          // the PSRAM window of the P4 (32 MB)
#define PSRAM_ADDR_END       0x4A000000u
#define PSRAM_PAGE           4096u
#define PSRAM_MAP_BASE       0x48400000u // the per-page failure map covers the scan window (6 MB = 1536 pages = 192 bytes)
#define PSRAM_MAP_PAGES      1536u

typedef struct {
    uint16_t version;
    uint16_t speed_mhz; // PSRAM clock the range was learned at: another speed starts over
    uint32_t lo, hi;    // lowest / highest bad byte, inclusive; lo > hi = nothing learned
    uint32_t writes;    // how many times this record was stored (shown in "fw psram", should stop growing)
    uint8_t  pages[PSRAM_MAP_PAGES / 8]; // bit set = this 4 KB page ever had a bad byte (any detector, any boot)
} psram_learn_t;

typedef enum {
    PSRAM_LEARN_SAME,    // inside the range already: nothing to store
    PSRAM_LEARN_GREW,    // the range grew: store it
    PSRAM_LEARN_OUTLIER, // not PSRAM, or reversed: ignored
} psram_learn_result_t;

// A fresh, empty record for this PSRAM speed
void psram_learn_reset(psram_learn_t *r, unsigned speed_mhz);

bool psram_learn_empty(psram_learn_t const *r);

// Is a record read back from NVS (size bytes) usable at this speed? Wrong size, version or speed = start over.
bool psram_learn_usable(psram_learn_t const *r, size_t size, unsigned speed_mhz);

// Folds the bad bytes [lo, hi] into the statistics and marks the pages of both ends. GREW (also when only a new page
// got marked) bumps writes: the caller stores the record.
psram_learn_result_t psram_learn_note(psram_learn_t *r, uintptr_t lo, uintptr_t hi);

// Marks the page holding addr in the failure map. true = a page that never failed before (the record needs storing).
bool psram_learn_mark_page(psram_learn_t *r, uintptr_t addr);

// Pages of the learned span [lo, hi]: how many, and how many of them ever failed
void psram_learn_page_counts(psram_learn_t const *r, unsigned *span_pages, unsigned *failed);

// '#' = failed, '.' = never failed, one character per page of the span (lo's page first); returns the length
size_t psram_learn_page_map(psram_learn_t const *r, char *out, size_t cap);

// Marks the pages of the recorded lowest/highest bad byte (they are failures themselves). true = the set changed.
bool psram_learn_seed(psram_learn_t *r);

// First failed page at or after from (page-aligned address) in the map window; false = none
bool psram_learn_next_failed(psram_learn_t const *r, uintptr_t from, uintptr_t *page);

// Number of failed pages in the set
unsigned psram_learn_failed_pages(psram_learn_t const *r);
