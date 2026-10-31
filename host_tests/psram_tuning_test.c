// Host test: PSRAM tuning capture, formatting, boot verdict and the reboot-once policy (#40)
#include "psram_tuning.h"

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
    uint8_t data, dqs;
    psram_tuning_delays(0, &data, &dqs);
    CHECK(data == 0 && dqs == 15);
    psram_tuning_delays(15, &data, &dqs);
    CHECK(data == 0 && dqs == 0);
    psram_tuning_delays(17, &data, &dqs);
    CHECK(data == 2 && dqs == 0);
    psram_tuning_delays(30, &data, &dqs);
    CHECK(data == 15 && dqs == 0);

    // Window 9..25 (len 17): IDF picks end - len/2 = 17
    psram_tuning_t t = {0};
    char           out[160];
    CHECK(psram_tuning_fell_back(&t)); // never ran
    psram_tuning_format(out, sizeof(out), &t);
    CHECK(strcmp(out, "tuning not captured") == 0);
    psram_tuning_note_phase(&t, 3, 3); // phases 1..3 passed -> first = 1
    CHECK(t.phase_id == 1);
    psram_tuning_note_phase(&t, 2, 3); // phases 2,3 -> id 2 = 90 degrees
    CHECK(t.phase_id == 2 && psram_tuning_phase_centi(t.phase_id) == 9000);
    psram_tuning_note_delayline(&t, 17, 25);
    CHECK(t.dl_idx == 17 && psram_tuning_dl_start(&t) == 9);
    CHECK(!psram_tuning_fell_back(&t));
    psram_tuning_format(out, sizeof(out), &t);
    CHECK(strcmp(out, "phase 90.00 (run 2/4), delayline idx 17 (data 2, dqs 0), window 9..25 len 17") == 0);

    // Fallbacks: delayline run <= 1 goes to idx 0, phase run 0 to id 0
    psram_tuning_note_delayline(&t, 1, 12);
    CHECK(t.dl_idx == 0 && psram_tuning_fell_back(&t));
    psram_tuning_note_delayline(&t, 0, 0);
    CHECK(t.dl_idx == 0 && psram_tuning_dl_start(&t) == 0);
    psram_tuning_note_delayline(&t, 17, 25);
    psram_tuning_note_phase(&t, 0, 0);
    CHECK(t.phase_id == 0 && psram_tuning_fell_back(&t));
    psram_tuning_format(out, sizeof(out), &t);
    CHECK(strstr(out, "FELL BACK") != NULL);

    // Verdict
    char why[80];
    psram_tuning_note_phase(&t, 2, 3);
    CHECK(!psram_boot_verdict(&t, 0, why, sizeof(why)) && why[0] == 0);
    CHECK(psram_boot_verdict(&t, 3, why, sizeof(why)) && strstr(why, "bad=3") && !strstr(why, "fallback"));
    psram_tuning_note_delayline(&t, 1, 5);
    CHECK(psram_boot_verdict(&t, 0, why, sizeof(why)) && strstr(why, "fallback") && strlen(why) < 60);

    // Reboot-once: one retry, then none until 10 min later; a good boot clears
    psram_retry_t st = {0};
    CHECK(!psram_retry_decide(&st, false, 1000));
    CHECK(psram_retry_decide(&st, true, 1000));
    CHECK(!psram_retry_decide(&st, true, 1100)); // still bad right after the retry
    CHECK(!psram_retry_decide(&st, true, 1599));
    CHECK(psram_retry_decide(&st, true, 1600));  // 10 minutes later
    CHECK(!psram_retry_decide(&st, false, 1700));
    CHECK(st.retries == 0);
    CHECK(psram_retry_decide(&st, true, 1800));  // fresh after a good boot
    // Clock never set: one retry, never a second until a good boot
    psram_retry_t nc = {0};
    CHECK(psram_retry_decide(&nc, true, 0));
    CHECK(!psram_retry_decide(&nc, true, 0));
    CHECK(!psram_retry_decide(&nc, true, 5000));
    CHECK(!psram_retry_decide(&nc, false, 0) && psram_retry_decide(&nc, true, 0));

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("psram_tuning_test ok\n");
    return 0;
}
