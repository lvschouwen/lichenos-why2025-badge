// Host test: boot step model (#66)
#include "boot_steps.h"

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
    boot_view_t v;
    boot_steps_reset();
    boot_steps_view(&v);
    for (int i = 0; i < BOOT_STEP_COUNT; i++) {
        CHECK(v.state[i] == STEP_PENDING);
    }
    CHECK(v.pct == -1);

    boot_step_begin(BOOT_C6);
    boot_steps_view(&v);
    CHECK(v.current == BOOT_C6);
    CHECK(v.state[BOOT_C6] == STEP_ACTIVE);

    boot_step_progress(BOOT_C6, 40);
    boot_steps_view(&v);
    CHECK(v.pct == 40);

    boot_step_end(BOOT_C6, STEP_DONE, NULL);
    boot_steps_view(&v);
    CHECK(v.state[BOOT_C6] == STEP_DONE);
    CHECK(v.pct == -1);
    CHECK(v.reason[BOOT_C6][0] == '\0');

    boot_step_end(BOOT_WIFI, STEP_SKIPPED, "no credentials");
    boot_steps_view(&v);
    CHECK(v.state[BOOT_WIFI] == STEP_SKIPPED);
    CHECK(strcmp(v.reason[BOOT_WIFI], "no credentials") == 0);

    boot_step_end(BOOT_LORA, STEP_FAILED, "C6 down");
    boot_steps_view(&v);
    CHECK(v.state[BOOT_LORA] == STEP_FAILED);
    CHECK(strcmp(v.reason[BOOT_LORA], "C6 down") == 0);

    // Progress of a step that is not running is ignored
    boot_step_progress(BOOT_APPS, 10);
    boot_steps_view(&v);
    CHECK(v.pct == -1);

    // Every call counts as a change
    uint32_t g = v.generation;
    boot_step_begin(BOOT_APPS);
    boot_steps_view(&v);
    CHECK(v.generation == g + 1);
    boot_step_progress(BOOT_APPS, 5);
    boot_steps_view(&v);
    CHECK(v.generation == g + 2);
    boot_step_end(BOOT_APPS, STEP_DONE, "");
    boot_steps_view(&v);
    CHECK(v.generation == g + 3);
    boot_steps_reset();
    boot_steps_view(&v);
    CHECK(v.generation == g + 4);
    CHECK(v.state[BOOT_APPS] == STEP_PENDING);

    char const *want[BOOT_STEP_COUNT] = {"storage", "display", "PSRAM", "C6", "Wi-Fi", "clock", "LoRa", "apps"};
    for (int i = 0; i < BOOT_STEP_COUNT; i++) {
        CHECK(strcmp(boot_step_label((boot_step_t)i), want[i]) == 0);
    }

    // 60 ASCII characters: cut to 47
    char const *sixty = "012345678901234567890123456789012345678901234567890123456789";
    boot_step_end(BOOT_STORAGE, STEP_FAILED, sixty);
    boot_steps_view(&v);
    CHECK(strlen(v.reason[BOOT_STORAGE]) == 47);
    CHECK(strncmp(v.reason[BOOT_STORAGE], sixty, 47) == 0);

    // Multi-byte: 46 ASCII then "é" (2 bytes) straddles byte 47, so the whole character goes
    char text[64];
    memset(text, 'a', 46);
    strcpy(text + 46, "\xC3\xA9\xC3\xA9");
    boot_step_end(BOOT_STORAGE, STEP_FAILED, text);
    boot_steps_view(&v);
    CHECK(strlen(v.reason[BOOT_STORAGE]) == 46);

    // "fw boot fail <step>": the saved flag is step + 1 (0 = none). PSRAM ends before NVS is up, so it can't be failed
    CHECK(boot_fail_step(0) == -1);
    CHECK(boot_fail_step(BOOT_STORAGE + 1) == BOOT_STORAGE);
    CHECK(boot_fail_step(BOOT_LORA + 1) == BOOT_LORA);
    CHECK(boot_fail_step(BOOT_APPS + 1) == BOOT_APPS);
    CHECK(boot_fail_step(BOOT_PSRAM + 1) == -1);
    CHECK(boot_fail_step(BOOT_STEP_COUNT + 1) == -1);
    CHECK(boot_fail_step(255) == -1);
    CHECK(boot_step_failable(BOOT_LORA));
    CHECK(boot_step_failable(BOOT_STORAGE));
    CHECK(!boot_step_failable(BOOT_PSRAM));

    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
