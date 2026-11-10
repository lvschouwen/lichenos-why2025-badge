// Host test: the per-app table slot lookup behind lichen_fs_app_dir (#272)
#include "app_slot.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define N 4

int main(void) {
    static int  a, b, c, d, e; // only their addresses count, as an app's descriptor does
    void const *owners[N] = {0};

    // An empty table hands out the first slot; once claimed, the same app gets the same slot back
    size_t i = app_slot_pick(owners, N, &a);
    CHECK(i == 0);
    owners[i] = &a;
    CHECK(app_slot_pick(owners, N, &a) == 0);
    CHECK(app_slot_pick(owners, N, &b) == 1);

    // An app's own slot wins over a free one before it, so it never claims two
    void const *holey[N] = {NULL, &b, &a, NULL};
    CHECK(app_slot_pick(holey, N, &a) == 2);
    CHECK(app_slot_pick(holey, N, &c) == 0);

    // A full table still finds its owners, and has no room for a new app
    void const *full[N] = {&a, &b, &c, &d};
    CHECK(app_slot_pick(full, N, &d) == 3);
    CHECK(app_slot_pick(full, N, &e) == N);

    // No table at all
    CHECK(app_slot_pick(owners, 0, &a) == 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("app_slot_test: all passed\n");
    return 0;
}
