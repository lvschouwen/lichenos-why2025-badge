#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// PSRAM quarantine (#86): the address ranges around cells that fail the boot scan are kept allocated forever, so no
// buffer ever lands on them. Pure range math, host-tested in host_tests/psram_quar_test.c; psram_boot.c feeds it.

#define PSRAM_QUAR_GRAIN  (4u * 1024u)   // ranges start and end on this (one page): the edges are claimed in 4 KB pieces
#define PSRAM_QUAR_MARGIN (4u * 1024u)   // one page each side of every failed page (Lucas); ranges that touch merge
#define PSRAM_QUAR_MAX    256            // separate ranges kept; beyond that the nearest ones are merged

typedef struct {
    uintptr_t lo, hi;         // quarantined [lo, hi), grain-aligned, margin included
    uintptr_t first_bad;      // lowest bad byte seen in it
    uintptr_t last_bad;       // highest bad byte seen in it
    uint32_t  bits;           // OR of the flipped bits, at their lane of the word
    uint32_t  bad_bytes;      // bad bytes noted, or failed pages when fed page by page
} psram_quar_range_t;

typedef struct {
    psram_quar_range_t r[PSRAM_QUAR_MAX + 1]; // sorted by lo, disjoint; the extra slot is working room
    size_t             n;
} psram_quar_t;

// Adds the bad bytes found in [first_bad, last_bad]. Widens by the margin, rounds to the grain, merges with ranges it
// touches or overlaps (and the two nearest ones when the table is full).
void psram_quar_note(psram_quar_t *q, uintptr_t first_bad, uintptr_t last_bad, uint32_t bad_bytes, uint32_t bits);

// Does [addr, addr + len) touch a quarantined range?
bool psram_quar_overlaps(psram_quar_t const *q, uintptr_t addr, size_t len);

// Total quarantined bytes
size_t psram_quar_bytes(psram_quar_t const *q);

// A block we hold: [lo, hi)
typedef struct {
    uintptr_t lo, hi;
} psram_span_t;

// Bytes of the quarantine that no span covers. spans must be sorted by lo. A gap shorter than slack between two spans (or
// at a range edge) is not counted: that is the allocator's own block header, which nobody can hand out. The claim in
// psram_scan.c is only guaranteed when this is 0.
size_t psram_quar_uncovered(psram_quar_t const *q, psram_span_t const *spans, size_t n, size_t slack);
