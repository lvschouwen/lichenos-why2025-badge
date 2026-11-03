// Host test: the Calculator's unit conversions (#158)
#include "calc_units.h"

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

static void conv(unit_conv_t c, double in, char const *want) {
    char       out[48];
    unit_err_t e = unit_convert(c, in, out, sizeof(out));
    if (e != UNIT_OK || strcmp(out, want) != 0) {
        printf("FAIL %s %g: err %d \"%s\" want \"%s\"\n", unit_conv_name(c), in, (int)e, out, want);
        failures++;
    }
}

static void num(char const *text, bool ok, double want) {
    double v = -7;
    bool   r = unit_parse_number(text, &v);
    if (r != ok || (ok && (v < want - 1e-9 || v > want + 1e-9))) {
        printf("FAIL parse \"%s\": %d %g\n", text, (int)r, v);
        failures++;
    }
}

int main(void) {
    conv(UNIT_DBM_TO_MW, 0, "1.000 mW");
    conv(UNIT_DBM_TO_MW, 10, "10.00 mW");
    conv(UNIT_DBM_TO_MW, 14, "25.12 mW");
    conv(UNIT_DBM_TO_MW, 22, "158.5 mW");
    conv(UNIT_DBM_TO_MW, 30, "1000 mW");
    conv(UNIT_DBM_TO_MW, -30, "0.001000 mW");
    conv(UNIT_MW_TO_DBM, 1, "0.0 dBm");
    conv(UNIT_MW_TO_DBM, 100, "20.0 dBm");
    conv(UNIT_MW_TO_DBM, 0.5, "-3.0 dBm");
    conv(UNIT_MHZ_TO_M, 868, "0.3454 m");
    conv(UNIT_MHZ_TO_M, 2400, "0.1249 m");
    conv(UNIT_MHZ_TO_M, 100, "2.998 m");
    conv(UNIT_M_TO_MHZ, 1, "299.8 MHz");
    conv(UNIT_M_TO_MHZ, 0.5, "599.6 MHz");
    conv(UNIT_C_TO_F, 0, "32.0 \xC2\xB0""F");
    conv(UNIT_C_TO_F, 100, "212.0 \xC2\xB0""F");
    conv(UNIT_C_TO_F, -40, "-40.0 \xC2\xB0""F");
    conv(UNIT_F_TO_C, 32, "0.0 \xC2\xB0""C");
    conv(UNIT_F_TO_C, 98.6, "37.0 \xC2\xB0""C");
    conv(UNIT_F_TO_C, -40, "-40.0 \xC2\xB0""C");

    char out[48];
    CHECK(unit_convert(UNIT_MW_TO_DBM, 0, out, sizeof(out)) == UNIT_NEEDS_POSITIVE);
    CHECK(unit_convert(UNIT_MW_TO_DBM, -5, out, sizeof(out)) == UNIT_NEEDS_POSITIVE);
    CHECK(unit_convert(UNIT_MHZ_TO_M, 0, out, sizeof(out)) == UNIT_NEEDS_POSITIVE);
    CHECK(unit_convert(UNIT_M_TO_MHZ, -1, out, sizeof(out)) == UNIT_NEEDS_POSITIVE);
    CHECK(unit_convert(UNIT_DBM_TO_MW, 1000, out, sizeof(out)) == UNIT_OUT_OF_RANGE);
    CHECK(unit_convert(UNIT_C_TO_F, 1e12, out, sizeof(out)) == UNIT_OUT_OF_RANGE);
    CHECK(unit_convert(UNIT_MHZ_TO_M, 1e9, out, sizeof(out)) == UNIT_OK && strlen(out) < 40);
    CHECK(unit_convert(UNIT_M_TO_MHZ, 1e-9, out, sizeof(out)) == UNIT_OK && strlen(out) < 40);
    CHECK(strcmp(unit_err_text(UNIT_NEEDS_POSITIVE, UNIT_MHZ_TO_M), "A frequency must be more than 0 MHz") == 0);
    CHECK(strcmp(unit_in_name(UNIT_MHZ_TO_M), "MHz") == 0);
    CHECK(unit_conv_name(UNIT_COUNT)[0] == '\0');

    num("14", true, 14);
    num(" -3.5 ", true, -3.5);
    num("+2", true, 2);
    num("0,25", true, 0.25);
    num(".5", true, 0.5);
    num("5.", true, 5);
    num("", false, 0);
    num("-", false, 0);
    num(".", false, 0);
    num("1e5", false, 0);
    num("0x10", false, 0);
    num("inf", false, 0);
    num("1 2", false, 0);
    num("12abc", false, 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("calc_units_test: ok\n");
    return 0;
}
