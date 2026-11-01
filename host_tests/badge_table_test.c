// Host test: the tile badge table behind lichen_badge.h (#102)
#include "badge_table.h"

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

int main(void) {
    badge_table_t t;
    memset(&t, 0, sizeof(t));

    // Set, get, change, clear; the generation moves only on a change
    CHECK(badge_table_get(&t, "crashlog") == 0);
    CHECK(!badge_table_set(&t, "crashlog", 0) && t.generation == 0); // none stays none
    CHECK(badge_table_set(&t, "crashlog", 1) && t.generation == 1);
    CHECK(badge_table_get(&t, "crashlog") == 1);
    CHECK(!badge_table_set(&t, "crashlog", 1) && t.generation == 1); // the same count again
    CHECK(badge_table_set(&t, "meshcore", 120) && t.generation == 2);
    CHECK(badge_table_get(&t, "meshcore") == 120 && badge_table_get(&t, "crashlog") == 1);
    CHECK(badge_table_set(&t, "crashlog", 3) && t.generation == 3);
    CHECK(badge_table_set(&t, "crashlog", 0) && t.generation == 4);
    CHECK(badge_table_get(&t, "crashlog") == 0 && badge_table_get(&t, "meshcore") == 120);

    // Ids that are no app id are ignored; an unknown valid id simply has none
    CHECK(!badge_table_set(&t, NULL, 5));
    CHECK(!badge_table_set(&t, "", 5));
    CHECK(!badge_table_set(&t, "Upper", 5));
    CHECK(!badge_table_set(&t, "fourteen_chars", 5));
    CHECK(t.generation == 4);
    CHECK(badge_table_get(&t, NULL) == 0 && badge_table_get(&t, "nosuchapp") == 0);

    // A cleared slot is reused; a full table ignores a new id but still updates the ones it holds
    char id[8];
    for (int i = 0; i < BADGE_SLOTS - 1; i++) {
        snprintf(id, sizeof(id), "app%d", i);
        CHECK(badge_table_set(&t, id, (unsigned)i + 1));
    }
    CHECK(!badge_table_set(&t, "onemore", 1) && badge_table_get(&t, "onemore") == 0);
    CHECK(badge_table_set(&t, "meshcore", 0));
    CHECK(badge_table_set(&t, "onemore", 7) && badge_table_get(&t, "onemore") == 7);
    CHECK(badge_table_get(&t, "app0") == 1 && badge_table_get(&t, "app14") == 15);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("badge_table_test: ok\n");
    return 0;
}
