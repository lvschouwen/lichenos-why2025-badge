// The journal's file generation (#368): whether the file the writer appends to is still the journal it started.
// ESP-free so the decision is host-tested.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MESH_JOURNAL_GEN_APPEND, // the journal is there: append to it
    MESH_JOURNAL_GEN_REOPEN, // gone (a format or a wiped card) or empty: write a new HEAD first, then append
    MESH_JOURNAL_GEN_FAIL,   // the card cannot say (I/O error, timeout, no device): fail the batch, never recover
} mesh_journal_gen_t;

// stat_ok: stat(log.bin) succeeded; err: its errno when it did not; size: the file's length when it did. Only a plain
// ENOENT means "gone". Any other error may be a card that is busy or failing with the journal still on it: the recovery
// path must not run then.
mesh_journal_gen_t mesh_journal_gen_plan(bool stat_ok, int err, uint64_t size);
