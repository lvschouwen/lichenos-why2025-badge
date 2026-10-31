// Host test: the calm boot screen's sentence and overall progress (#66)
#include "boot_text.h"

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

static char const *sentence(boot_step_t step, int pct) {
    static char buf[64];
    return boot_step_sentence(step, pct, buf, sizeof(buf));
}

int main(void) {
    CHECK(strcmp(sentence(BOOT_STORAGE, -1), "Opening storage...") == 0);
    CHECK(strcmp(sentence(BOOT_DISPLAY, -1), "Starting the screen...") == 0);
    CHECK(strcmp(sentence(BOOT_PSRAM, -1), "Checking memory...") == 0);
    CHECK(strcmp(sentence(BOOT_C6, -1), "Waking the radio chip...") == 0);
    CHECK(strcmp(sentence(BOOT_C6, 40), "Updating the radio chip (40 %)...") == 0);
    CHECK(strcmp(sentence(BOOT_WIFI, -1), "Connecting to Wi-Fi...") == 0);
    CHECK(strcmp(sentence(BOOT_CLOCK, -1), "Setting the clock...") == 0);
    CHECK(strcmp(sentence(BOOT_LORA, -1), "Starting LoRa...") == 0);
    CHECK(strcmp(sentence(BOOT_APPS, -1), "Starting apps...") == 0);
    CHECK(strcmp(sentence(BOOT_WIFI, 40), "Connecting to Wi-Fi...") == 0); // only the C6 shows a percentage

    char small[8];
    CHECK(boot_step_sentence(BOOT_C6, 40, small, sizeof(small)) == small); // truncated, never overruns
    CHECK(strlen(small) == 7);

    boot_view_t v;
    memset(&v, 0, sizeof(v)); // all PENDING
    CHECK(boot_overall_pct(&v) == 0);

    for (int i = 0; i < BOOT_STEP_COUNT; i++) {
        v.state[i] = i == BOOT_CLOCK ? STEP_PENDING : STEP_DONE;
    }
    CHECK(boot_overall_pct(&v) == 100); // the clock waits on SNTP and never blocks the bar

    v.state[BOOT_LORA] = STEP_ACTIVE;
    v.state[BOOT_APPS] = STEP_PENDING;
    CHECK(boot_overall_pct(&v) == 5 * 100 / 7);
    v.state[BOOT_LORA] = STEP_FAILED;
    v.state[BOOT_APPS] = STEP_SKIPPED;
    CHECK(boot_overall_pct(&v) == 100); // failed and skipped steps count as over

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("boot_text_test: all passed\n");
    return 0;
}
