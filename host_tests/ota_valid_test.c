// Host test: when a freshly installed lean firmware (#61) may cancel the rollback
#include "ota_valid.h"

#include <stdint.h>
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
    uint32_t const settled = OTA_VALID_AFTER_S;
    CHECK(ota_should_mark_valid(true, true, true, settled));
    CHECK(ota_should_mark_valid(true, true, true, settled + 100));

    // Not yet: a build that crashes seconds after boot must still roll back
    CHECK(!ota_should_mark_valid(true, true, true, 0));
    CHECK(!ota_should_mark_valid(true, true, true, settled - 1));

    // Not yet: the UI or the C6 link isn't up
    CHECK(!ota_should_mark_valid(true, false, true, settled));
    CHECK(!ota_should_mark_valid(true, true, false, settled));
    CHECK(!ota_should_mark_valid(true, false, false, settled));

    // Nothing to decide for an image that isn't on trial
    CHECK(!ota_should_mark_valid(false, true, true, settled));

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ota_valid_test: all checks passed\n");
    return 0;
}
