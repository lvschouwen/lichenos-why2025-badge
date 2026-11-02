// Host test: the update server's optional auto-off (#44, #110)
#include "ota_autooff.h"

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
    // Settings steps Never -> 10 -> 30 -> 60 -> 120 -> Never; a console value steps to the next choice above it
    CHECK(ota_autooff_next(0) == 10 && ota_autooff_next(10) == 30 && ota_autooff_next(30) == 60);
    CHECK(ota_autooff_next(60) == 120 && ota_autooff_next(120) == 0);
    CHECK(ota_autooff_next(45) == 60 && ota_autooff_next(200) == 0);

    char text[16];
    ota_autooff_text(0, text, sizeof(text));
    CHECK(strcmp(text, "Never") == 0);
    ota_autooff_text(10, text, sizeof(text));
    CHECK(strcmp(text, "10 min") == 0);
    ota_autooff_text(120, text, sizeof(text));
    CHECK(strcmp(text, "2 h") == 0);

    uint8_t m = 99;
    CHECK(ota_autooff_parse("off", &m) && m == 0);
    CHECK(ota_autooff_parse("30", &m) && m == 30);
    CHECK(ota_autooff_parse("240", &m) && m == 240);
    CHECK(!ota_autooff_parse("241", &m) && !ota_autooff_parse("0", &m) && !ota_autooff_parse("", &m));
    CHECK(!ota_autooff_parse("1000", &m) && !ota_autooff_parse("10m", &m) && !ota_autooff_parse("-5", &m));

    // Off by default: never due
    CHECK(!ota_autooff_due(0, 0, 1000000, false));
    // 10 minutes after the arming (boot at 0, or a switch-on later), not before
    CHECK(!ota_autooff_due(10, 0, 599, false) && ota_autooff_due(10, 0, 600, false));
    CHECK(!ota_autooff_due(10, 5000, 5599, false) && ota_autooff_due(10, 5000, 5600, false));
    CHECK(ota_autooff_left(10, 5000, 5100) == 500 && ota_autooff_left(10, 5000, 9000) == 0);
    CHECK(ota_autooff_left(0, 0, 10) == 0);
    // An upload in progress keeps it open until it is done
    CHECK(!ota_autooff_due(10, 0, 700, true));
    // An arming time after now (never expected) counts as just armed
    CHECK(!ota_autooff_due(10, 900, 100, false) && ota_autooff_left(10, 900, 100) == 600);
    // The longest console value without overflow
    CHECK(!ota_autooff_due(240, 0, 240 * 60 - 1, false) && ota_autooff_due(240, 0, 240 * 60, false));

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ota_autooff_test: all checks passed\n");
    return 0;
}
