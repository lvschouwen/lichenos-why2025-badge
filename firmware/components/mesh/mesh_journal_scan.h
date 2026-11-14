// The journal read check's decisions (#120, "fw mesh hist scan"): which files it takes and what each pass reads.
// ESP-free so they are host-tested (#212).
#pragma once

#include "mesh_hist.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MESH_JOURNAL_SCAN_GO,          // run the passes
    MESH_JOURNAL_SCAN_MISSING,     // no such file, or an empty one
    MESH_JOURNAL_SCAN_TOO_BIG,     // past the scan's own limit: two raw copies of it would not fit
    MESH_JOURNAL_SCAN_INT_TOO_BIG, // "int" asked, and two copies would not fit the internal RAM quota
} mesh_journal_scan_t;

// found: stat() succeeded; size: the file's length then (64 bits: a card file past 2 GB must not wrap into range)
mesh_journal_scan_t mesh_journal_scan_plan(bool found, int64_t size, bool internal, int64_t max_bytes,
                                           int64_t int_max_bytes);

// The file offset pass p (1-based) reads from: pass 1 reads all of it, the later passes from skip on
long mesh_journal_scan_skip(int pass, long skip);

// Whether pass p first copies pass 1's head (the bytes it skips) into its buffer: once, before pass 2, so the skipped
// head compares equal with itself
bool mesh_journal_scan_copies_head(int pass, long skip, int64_t size);

// A pass that found damage: a bad spot, a torn end or a record that did not authenticate
bool mesh_journal_scan_bad(mesh_hist_scan_t const *s);
