// Host test: the shim's CayenneLPP encodes MeshCore's telemetry byte for byte as electroniccats/CayenneLPP 1.6.1 (#65)
#include "CayenneLPP.h"

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
    CayenneLPP lpp(10);
    CHECK(lpp.getSize() == 0);

    // channel, type 116 (0.01 V unsigned), MSB first: 4.17 V -> 417 = 0x01A1
    CHECK(lpp.addVoltage(1, 4.17f) == 4);
    // channel, type 103 (0.1 degC signed): -12.5 degC -> -125 = 0xFF83
    CHECK(lpp.addTemperature(1, -12.5f) == 8);
    uint8_t const expect[] = {1, 116, 0x01, 0xA1, 1, 103, 0xFF, 0x83};
    CHECK(lpp.getSize() == sizeof(expect) && memcmp(lpp.getBuffer(), expect, sizeof(expect)) == 0);

    // Full: nothing written, 0 returned
    CHECK(lpp.addVoltage(2, 3.3f) == 0 && lpp.getSize() == 8);

    lpp.reset();
    CHECK(lpp.getSize() == 0);
    CHECK(lpp.addTemperature(3, 21.37f) == 4 && lpp.getBuffer()[2] == 0x00 && lpp.getBuffer()[3] == 213);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("cayenne_lpp_test: all checks passed\n");
    return 0;
}
