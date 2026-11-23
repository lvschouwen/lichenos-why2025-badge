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

// Whether an id is one of the two ranges of the apps' screens, SCREEN_APP_OWN + 1..255 and SCREEN_APP_BASE + 1..255
static int in_app_range(int value) {
    return (value > SCREEN_APP_OWN && value <= SCREEN_APP_OWN + SCREEN_APP_MAX) ||
           (value > SCREEN_APP_BASE && value <= SCREEN_APP_BASE + SCREEN_APP_MAX);
}

int main(void) {
    size_t n = sizeof(ids) / sizeof(ids[0]);
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (ids[i].value == ids[j].value) { // cppcheck-suppress arrayIndexOutOfBounds ; it cannot expand SCREEN_ID_LIST
                printf("duplicate id %d: %s and %s\n", ids[i].value, ids[i].name, ids[j].name);
                failures++;
            }
        }
        // shell_open_app uses SCREEN_APP_OWN + slot; the apps' pages use SCREEN_APP_BASE + slot
        CHECK(!in_app_range(ids[i].value));
    }
    CHECK(SCREEN_APP_OWN + SCREEN_APP_MAX < SCREEN_APP_BASE); // the two app ranges never meet
    CHECK(in_app_range(SCREEN_APP_OWN + 1) && in_app_range(SCREEN_APP_BASE + SCREEN_APP_MAX) && !in_app_range(SCREEN_APP_OWN));
    CHECK(SCREEN_APP_MAX == 255);                              // one id per slot of the 8-bit context handle
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("screen_ids_test: %d ids, all unique\n", (int)n);
    return 0;
}
