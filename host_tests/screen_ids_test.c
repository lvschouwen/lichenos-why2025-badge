// Host test: every screen id is unique and clear of the app ranges (#148: a screen is (id, key))
#include "screen_ids.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define VALUE(name, value) {#name, value},
static struct {
    char const *name;
    int         value;
} const ids[] = {SCREEN_ID_LIST(VALUE)};

int main(void) {
    int n = (int)(sizeof(ids) / sizeof(ids[0]));
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (ids[i].value == ids[j].value) {
                printf("duplicate id %d: %s and %s\n", ids[i].value, ids[i].name, ids[j].name);
                failures++;
            }
        }
        // shell_open_app uses 1 + index; the apps' pages use SCREEN_APP_BASE + index
        CHECK(ids[i].value > SCREEN_APP_MAX);
        CHECK(ids[i].value < SCREEN_APP_BASE || ids[i].value >= SCREEN_APP_BASE + SCREEN_APP_MAX);
    }
    CHECK(SCREEN_APP_BASE > SCREEN_APP_MAX);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("screen_ids_test: %d ids, all unique\n", n);
    return 0;
}
