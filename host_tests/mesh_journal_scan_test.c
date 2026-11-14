// Host test: the journal read check's decisions (#120, #212): which files it takes and what each pass reads
#include "mesh_journal_scan.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define MAX (1024 * 1024)
#define INT ((64 * 1024) / 2 - 1024)

static void plan(void) {
    CHECK(mesh_journal_scan_plan(false, 0, false, MAX, INT) == MESH_JOURNAL_SCAN_MISSING);
    CHECK(mesh_journal_scan_plan(false, 100, true, MAX, INT) == MESH_JOURNAL_SCAN_MISSING); // size unread: ignored
    CHECK(mesh_journal_scan_plan(true, 0, false, MAX, INT) == MESH_JOURNAL_SCAN_MISSING);
    CHECK(mesh_journal_scan_plan(true, -1, false, MAX, INT) == MESH_JOURNAL_SCAN_MISSING);
    CHECK(mesh_journal_scan_plan(true, 1, false, MAX, INT) == MESH_JOURNAL_SCAN_GO);
    CHECK(mesh_journal_scan_plan(true, MAX, false, MAX, INT) == MESH_JOURNAL_SCAN_GO);
    CHECK(mesh_journal_scan_plan(true, MAX + 1, false, MAX, INT) == MESH_JOURNAL_SCAN_TOO_BIG);
    CHECK(mesh_journal_scan_plan(true, MAX + 1, true, MAX, INT) == MESH_JOURNAL_SCAN_TOO_BIG); // the general limit first
    CHECK(mesh_journal_scan_plan(true, INT, true, MAX, INT) == MESH_JOURNAL_SCAN_GO);
    CHECK(mesh_journal_scan_plan(true, INT + 1, true, MAX, INT) == MESH_JOURNAL_SCAN_INT_TOO_BIG);
    CHECK(mesh_journal_scan_plan(true, INT + 1, false, MAX, INT) == MESH_JOURNAL_SCAN_GO);
    // a file past 4 GB does not wrap into range
    CHECK(mesh_journal_scan_plan(true, (int64_t)4294967296LL + 10, false, MAX, INT) == MESH_JOURNAL_SCAN_TOO_BIG);
}

static void passes(void) {
    CHECK(mesh_journal_scan_skip(1, 700) == 0);
    CHECK(mesh_journal_scan_skip(2, 700) == 700 && mesh_journal_scan_skip(9, 700) == 700);
    CHECK(mesh_journal_scan_skip(2, 0) == 0);
    CHECK(mesh_journal_scan_copies_head(2, 700, 1000));
    CHECK(!mesh_journal_scan_copies_head(1, 700, 1000));
    CHECK(!mesh_journal_scan_copies_head(3, 700, 1000)); // copied once: later passes keep it in cur
    CHECK(!mesh_journal_scan_copies_head(2, 0, 1000));
    CHECK(!mesh_journal_scan_copies_head(2, -5, 1000));
    CHECK(!mesh_journal_scan_copies_head(2, 1000, 1000)); // past the file: nothing to skip
    CHECK(mesh_journal_scan_copies_head(2, 999, 1000));
}

static void bad(void) {
    mesh_hist_scan_t s;
    memset(&s, 0, sizeof(s));
    s.records = 10, s.skipped_bytes = 3, s.bad_seen = true, s.unreadable = true; // none of these alone counts
    CHECK(!mesh_journal_scan_bad(&s));
    s.resyncs = 1;
    CHECK(mesh_journal_scan_bad(&s));
    s.resyncs = 0, s.torn = true;
    CHECK(mesh_journal_scan_bad(&s));
    s.torn = false, s.auth_failures = 2;
    CHECK(mesh_journal_scan_bad(&s));
}

int main(void) {
    plan();
    passes();
    bad();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_journal_scan_test: all checks passed\n");
    return 0;
}
