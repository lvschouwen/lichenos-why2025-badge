// Host test: the journal's reopen decision after the card was formatted or wiped (#368)
#include "mesh_journal_gen.h"

#include <stdio.h>

int main(void) {
    int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
    CHECK(mesh_journal_gen_plan(false, 0) == MESH_JOURNAL_GEN_REOPEN);      // formatted: no log.bin (errno 2 before)
    CHECK(mesh_journal_gen_plan(false, 4096) == MESH_JOURNAL_GEN_REOPEN);   // a size without a file means nothing
    CHECK(mesh_journal_gen_plan(true, 0) == MESH_JOURNAL_GEN_REOPEN);       // an empty file has no HEAD
    CHECK(mesh_journal_gen_plan(true, 1) == MESH_JOURNAL_GEN_APPEND);       // never truncate bytes that exist
    CHECK(mesh_journal_gen_plan(true, 251) == MESH_JOURNAL_GEN_APPEND);
    CHECK(mesh_journal_gen_plan(true, UINT64_MAX) == MESH_JOURNAL_GEN_APPEND);
    if (failures) {
        return 1;
    }
    printf("mesh_journal_gen_test: all checks passed\n");
    return 0;
}
