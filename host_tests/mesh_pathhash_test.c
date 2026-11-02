// Host test: the one-shot move to 2 byte path hashes (#129)
#include "mesh_pathhash.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    bool mark;
    // First boot of this build, still on MeshCore's 1 B default (a fresh badge, or an old one): to 2 B, flag written
    CHECK(mesh_pathhash_migrate(false, 1, &mark) == 2 && mark);
    // 2 B or 3 B chosen already: left alone, flag written so a later 1 B stays
    CHECK(mesh_pathhash_migrate(false, 2, &mark) == 0 && mark);
    CHECK(mesh_pathhash_migrate(false, 3, &mark) == 0 && mark);
    // Done before: an explicit choice of 1 B sticks, nothing to write
    CHECK(mesh_pathhash_migrate(true, 1, &mark) == 0 && !mark);
    CHECK(mesh_pathhash_migrate(true, 2, &mark) == 0 && !mark);
    CHECK(MESH_PATH_HASH_DEFAULT == 2);
    if (failures) {
        return 1;
    }
    printf("mesh_pathhash_test: all checks passed\n");
    return 0;
}
