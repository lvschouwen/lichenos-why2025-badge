#include "mesh_journal_gen.h"

mesh_journal_gen_t mesh_journal_gen_plan(bool have, uint64_t size) {
    return (!have || size == 0) ? MESH_JOURNAL_GEN_REOPEN : MESH_JOURNAL_GEN_APPEND;
}
