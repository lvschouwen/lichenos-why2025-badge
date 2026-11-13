// The journal's file generation (#368): whether the file the writer appends to is still the journal it started.
// ESP-free so the decision is host-tested.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MESH_JOURNAL_GEN_APPEND,   // the journal is there: append to it
    MESH_JOURNAL_GEN_REOPEN,   // gone (a format or a wiped card) or empty: write a new HEAD first, then append
} mesh_journal_gen_t;

// have: log.bin was found; size: its length when found. A file with some bytes is never reopened here: a short or torn
// head is the boot's to compact or set aside, and "wb" must only ever truncate a file that holds nothing.
mesh_journal_gen_t mesh_journal_gen_plan(bool have, uint64_t size);
