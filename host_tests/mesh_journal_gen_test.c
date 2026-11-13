// Host test: the journal's reopen decision after the card was formatted or wiped (#368)
#include "mesh_journal_gen.h"

#include <errno.h>
#include <stdio.h>

int main(void) {
    int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
    CHECK(mesh_journal_gen_plan(false, ENOENT, 0) == MESH_JOURNAL_GEN_REOPEN); // formatted: no log.bin (errno 2)
    CHECK(mesh_journal_gen_plan(false, EIO, 0) == MESH_JOURNAL_GEN_FAIL);      // the card cannot say: never recover
    CHECK(mesh_journal_gen_plan(false, ETIMEDOUT, 0) == MESH_JOURNAL_GEN_FAIL);
    CHECK(mesh_journal_gen_plan(false, ENODEV, 0) == MESH_JOURNAL_GEN_FAIL);
    CHECK(mesh_journal_gen_plan(false, 0, 0) == MESH_JOURNAL_GEN_FAIL);        // a failed stat with no errno is no ENOENT
    CHECK(mesh_journal_gen_plan(true, 0, 0) == MESH_JOURNAL_GEN_REOPEN);       // an empty file has no HEAD
    CHECK(mesh_journal_gen_plan(true, 0, 1) == MESH_JOURNAL_GEN_APPEND);
    CHECK(mesh_journal_gen_plan(true, 0, 251) == MESH_JOURNAL_GEN_APPEND);
    CHECK(mesh_journal_gen_plan(true, ENOENT, UINT64_MAX) == MESH_JOURNAL_GEN_APPEND); // errno means nothing when stat worked
    if (failures) {
        return 1;
    }
    printf("mesh_journal_gen_test: all checks passed\n");
    return 0;
}
