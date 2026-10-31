// Host test: TCA8418 KP_GPIO1..3 masks (#77). The old driver read ROW_COL_BIT_MASK[MAX(cols, 8) - 1], out of bounds for cols=10.
#include "tca8418_mask.h"

#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void) {
    // The badge: create(..., 0, 0) means 8 rows x 10 columns
    tca8418_masks_t m = tca8418_key_masks(0, 0);
    CHECK(m.kp_gpio1 == 0xFF && m.kp_gpio2 == 0xFF && m.kp_gpio3 == 0x03);
    m = tca8418_key_masks(8, 10);
    CHECK(m.kp_gpio1 == 0xFF && m.kp_gpio2 == 0xFF && m.kp_gpio3 == 0x03);
    m = tca8418_key_masks(4, 4);
    CHECK(m.kp_gpio1 == 0x0F && m.kp_gpio2 == 0x0F && m.kp_gpio3 == 0x00);
    m = tca8418_key_masks(3, 9);
    CHECK(m.kp_gpio1 == 0x07 && m.kp_gpio2 == 0xFF && m.kp_gpio3 == 0x01);
    m = tca8418_key_masks(20, 20);
    CHECK(m.kp_gpio1 == 0xFF && m.kp_gpio2 == 0xFF && m.kp_gpio3 == 0x03);

    // #14: the KP_GPIO1 readback. A chip reset with the C6 reads its power-on 0; a failed read is no verdict.
    CHECK(tca8418_check_kp_gpio1(true, 0xFF, 0, 0) == TCA8418_CHECK_OK);
    CHECK(tca8418_check_kp_gpio1(true, 0x00, 0, 0) == TCA8418_CHECK_RESET);
    CHECK(tca8418_check_kp_gpio1(true, 0x0F, 0, 0) == TCA8418_CHECK_RESET);
    CHECK(tca8418_check_kp_gpio1(true, 0x0F, 4, 4) == TCA8418_CHECK_OK);
    CHECK(tca8418_check_kp_gpio1(false, 0x00, 0, 0) == TCA8418_CHECK_NO_ANSWER);
    CHECK(tca8418_check_kp_gpio1(false, 0xFF, 0, 0) == TCA8418_CHECK_NO_ANSWER);

    // #14: I2C warnings while the chip is held in reset: the first failure, then 100, 1000, ..., then "back"
    tca8418_warn_t w = {0};
    uint32_t       fails = 7;
    CHECK(tca8418_warn_ok(&w, &fails) == TCA8418_WARN_QUIET); // all fine: nothing to say
    CHECK(tca8418_warn_fail(&w) == TCA8418_WARN_FIRST);
    int logged = 1;
    for (uint32_t i = 2; i <= 20000; i++) {
        tca8418_warn_result_t r = tca8418_warn_fail(&w);
        if (r != TCA8418_WARN_QUIET) {
            CHECK(r == TCA8418_WARN_STILL);
            CHECK(i == 100 || i == 1000 || i == 10000);
            logged++;
        }
    }
    CHECK(logged == 4);
    CHECK(w.fails == 20000);
    CHECK(tca8418_warn_ok(&w, &fails) == TCA8418_WARN_BACK && fails == 20000);
    CHECK(tca8418_warn_ok(&w, &fails) == TCA8418_WARN_QUIET && w.fails == 0);
    CHECK(tca8418_warn_fail(&w) == TCA8418_WARN_FIRST); // a new run of failures is reported again
    return failures ? 1 : 0;
}
