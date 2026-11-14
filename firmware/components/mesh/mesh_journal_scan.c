// The journal read check's decisions (#120): see mesh_journal_scan.h
#include "mesh_journal_scan.h"

mesh_journal_scan_t mesh_journal_scan_plan(bool found, int64_t size, bool internal, int64_t max_bytes,
                                           int64_t int_max_bytes) {
    if (!found || size <= 0) {
        return MESH_JOURNAL_SCAN_MISSING;
    }
    if (size > max_bytes) {
        return MESH_JOURNAL_SCAN_TOO_BIG;
    }
    return internal && size > int_max_bytes ? MESH_JOURNAL_SCAN_INT_TOO_BIG : MESH_JOURNAL_SCAN_GO;
}

long mesh_journal_scan_skip(int pass, long skip) {
    return pass == 1 ? 0 : skip;
}

bool mesh_journal_scan_copies_head(int pass, long skip, int64_t size) {
    return pass == 2 && skip > 0 && skip < size;
}

bool mesh_journal_scan_bad(mesh_hist_scan_t const *s) {
    return s->resyncs || s->torn || s->auth_failures;
}
