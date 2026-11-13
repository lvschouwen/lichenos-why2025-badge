#include "mesh_journal_gen.h"

#include <errno.h>

mesh_journal_gen_t mesh_journal_gen_plan(bool stat_ok, int err, uint64_t size) {
    if (!stat_ok) {
        return err == ENOENT ? MESH_JOURNAL_GEN_REOPEN : MESH_JOURNAL_GEN_FAIL;
    }
    return size == 0 ? MESH_JOURNAL_GEN_REOPEN : MESH_JOURNAL_GEN_APPEND;
}
