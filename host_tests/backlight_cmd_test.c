// Host tests for the C6 backlight command parser and PWM duty
#include "backlight_cmd.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    int d = -1, k = -1;

    CHECK(backlight_cmd_parse("BL 30 10", &d, &k) && d == 30 && k == 10);
    CHECK(backlight_cmd_parse("BL 0 0", &d, &k) && d == 0 && k == 0);
    CHECK(backlight_cmd_parse("BL 100 100", &d, &k) && d == 100 && k == 100);
    CHECK(backlight_cmd_parse("BL 30 10 ", &d, &k)); // trailing space is harmless

    d = k = -1;
    CHECK(!backlight_cmd_parse("BL 101 10", &d, &k));
    CHECK(!backlight_cmd_parse("BL -1 10", &d, &k));
    CHECK(!backlight_cmd_parse("BL 30", &d, &k));
    CHECK(!backlight_cmd_parse("BL 30 10 5", &d, &k));
    CHECK(!backlight_cmd_parse("XL 30 10", &d, &k));
    CHECK(!backlight_cmd_parse("", &d, &k));
    CHECK(!backlight_cmd_parse("I (123) boot: BL 30 10", &d, &k));
    CHECK(d == -1 && k == -1); // untouched on failure

    // 8-bit PWM: off stays off, 1-9% is raised to 10%, anything above 80% is held at 80%
    CHECK(backlight_duty(0, 8) == 0);
    CHECK(backlight_duty(1, 8) == 25);
    CHECK(backlight_duty(10, 8) == 25);
    CHECK(backlight_duty(30, 8) == 76);
    CHECK(backlight_duty(80, 8) == 204);
    CHECK(backlight_duty(100, 8) == 204);
    CHECK(backlight_duty(-5, 8) == 0);
    CHECK(backlight_duty(50, 10) == 512);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("All backlight_cmd tests passed\n");
    return 0;
}
