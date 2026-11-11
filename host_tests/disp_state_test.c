// Host test: the display fingerprint line (#354)
#include "disp_state_fmt.h"

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

static void test_all(void) {
    ds_info_t in = {"sw", "fw-abc1234", "120000000 (1.0 s ago)", 120, DS_TRIAL_PASSED, 0, "36000000", "468 (3.0 s ago)",
                    40, 0, "40 (1.0 s ago)", "0", "on", "bright", "none", "1000 (2.0 s ago)", 1, 2, 65};
    char      out[256];
    size_t    n = ds_format(&in, out, sizeof(out));
    CHECK(strcmp(out, "disp state: boot sw fw-abc1234 | psram 120 want 120 trial passed failed no | pclk 36000000 "
                      "refresh 468 | bl 40/40 kbd 0/0 boot on idle bright override none freq 1000 | c6 up restarts 2 "
                      "| uptime 65") == 0);
    CHECK(n == strlen(out));
    in.failed = 1; in.trial = DS_TRIAL_ARMED; in.c6_up = 0; in.psram_clk_raw = "80000000";
    ds_format(&in, out, sizeof(out));
    CHECK(strstr(out, "psram 80 want 120 trial armed failed yes") && strstr(out, "c6 down"));
}

static void test_missing(void) {
    ds_info_t in = {NULL, NULL, "? (not read yet)", -1, -1, -1, NULL, "", -1, -1, "stale?", "?", NULL, NULL, NULL, NULL, -1, -1, -1};
    char      out[256];
    ds_format(&in, out, sizeof(out));
    CHECK(strcmp(out, "disp state: boot ? fw-? | psram ? want ? trial ? failed ? | pclk ? refresh ? | bl ?/stale? "
                      "kbd ?/? boot ? idle ? override ? freq ? | c6 ? restarts ? | uptime ?") == 0);
    in.psram_clk_raw = "abc";
    in.trial = 9;
    ds_format(&in, out, sizeof(out));
    CHECK(strstr(out, "psram ? want ? trial ?"));
}

static void test_cut(void) {
    ds_info_t in = {0};
    char      out[16];
    size_t    n = ds_format(&in, out, sizeof(out));
    CHECK(n == 15 && strlen(out) == 15);
    CHECK(ds_format(&in, out, 0) == 0);
}

int main(void) {
    test_all();
    test_missing();
    test_cut();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
