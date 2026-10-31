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
    return failures ? 1 : 0;
}
