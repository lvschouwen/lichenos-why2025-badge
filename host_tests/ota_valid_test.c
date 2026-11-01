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

static void test_image_header(void) {
    uint8_t head[OTA_IMAGE_HEADER_LEN] = {0xE9, 5};
    head[12] = 0x12; // ESP32-P4
    CHECK(ota_image_header_ok(head, sizeof(head), 0x0012));
    CHECK(!ota_image_header_ok(head, sizeof(head), 0x000D)); // a C6 badge wants a C6 image
    CHECK(!ota_image_header_ok(head, sizeof(head) - 1, 0x0012));
    CHECK(!ota_image_header_ok(NULL, 0, 0x0012));
    head[0] = 0x50; // "PK": a zip, not an image
    CHECK(!ota_image_header_ok(head, sizeof(head), 0x0012));
    head[0]  = 0xE9;
    head[12] = 0x0D; // a C6 image pushed to the P4
    CHECK(!ota_image_header_ok(head, sizeof(head), 0x0012));
}

int main(void) {
    test_image_header();
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

    // Rollback (#36): no C6 link on a trial image restarts it, but only after the grace time
    uint32_t const give_up = OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S;
    CHECK(ota_should_roll_back_now(true, false, give_up));
    CHECK(ota_should_roll_back_now(true, false, give_up + 1000));
    CHECK(!ota_should_roll_back_now(true, false, give_up - 1));
    CHECK(!ota_should_roll_back_now(true, false, 0));
    CHECK(!ota_should_roll_back_now(true, true, give_up + 1000)); // C6 up: Wi-Fi works, fix it over the air
    CHECK(!ota_should_roll_back_now(false, false, give_up + 1000)); // not on trial

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ota_valid_test: all checks passed\n");
    return 0;
}
