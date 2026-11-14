// Host test: the first-run setup steps (#66)
#include "setup_state.h"

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
    CHECK(setup_first(true) == SETUP_DONE);
    CHECK(setup_first(false) == SETUP_WELCOME);
    // A badge with Wi-Fi credentials sees the welcome but skips the Wi-Fi step
    CHECK(setup_next(SETUP_WELCOME, true) == SETUP_DONE);
    CHECK(setup_next(SETUP_WELCOME, false) == SETUP_WIFI);
    // Joined or skipped, the Wi-Fi step ends the setup
    CHECK(setup_next(SETUP_WIFI, false) == SETUP_DONE);
    CHECK(setup_next(SETUP_WIFI, true) == SETUP_DONE);
    CHECK(setup_next(SETUP_DONE, false) == SETUP_DONE);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("setup_state_test: ok\n");
    return 0;
}
