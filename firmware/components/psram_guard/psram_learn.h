#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Learned extent of the bad PSRAM bytes (#86): the lowest and highest bad address any detector ever saw, kept in NVS so
// the quarantine can shrink to what is really bad (plus a small margin) instead of a wide margin around one boot's
// scan. Pure logic (merge, plausibility, persistence decision), host-tested in host_tests/psram_learn_test.c;
// psram_learned.c holds it in NVS.

#define PSRAM_LEARN_VERSION  1
#define PSRAM_LEARN_MAX_SPAN (3u * 1024u * 1024u) // a find that would stretch the range past this is an outlier, not learned
#define PSRAM_ADDR_MIN       0x48000000u          // the PSRAM window of the P4 (32 MB)
#define PSRAM_ADDR_END       0x4A000000u

typedef struct {
    uint16_t version;
    uint16_t speed_mhz; // PSRAM clock the range was learned at: another speed starts over
    uint32_t lo, hi;    // lowest / highest bad byte, inclusive; lo > hi = nothing learned
    uint32_t writes;    // how many times this record was stored (shown in "fw psram", should stop growing)
} psram_learn_t;

typedef enum {
    PSRAM_LEARN_SAME,    // inside the range already: nothing to store
    PSRAM_LEARN_GREW,    // the range grew: store it
    PSRAM_LEARN_OUTLIER, // not PSRAM, or too far from the rest: ignored
} psram_learn_result_t;

// A fresh, empty record for this PSRAM speed
void psram_learn_reset(psram_learn_t *r, unsigned speed_mhz);

bool psram_learn_empty(psram_learn_t const *r);

// Is a record read back from NVS (size bytes) usable at this speed? Wrong size, version or speed = start over.
bool psram_learn_usable(psram_learn_t const *r, size_t size, unsigned speed_mhz);

// Folds the bad bytes [lo, hi] into the range. GREW also bumps writes (the caller stores the record).
psram_learn_result_t psram_learn_note(psram_learn_t *r, uintptr_t lo, uintptr_t hi);
