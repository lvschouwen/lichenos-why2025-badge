#pragma once

#include <stdbool.h>
#include <stddef.h>

// The Calculator's unit conversions (#158), pure so host_tests/calc_units_test.c covers them: radio power, frequency to
// wavelength and temperature.
typedef enum {
    UNIT_DBM_TO_MW,
    UNIT_MW_TO_DBM,
    UNIT_MHZ_TO_M,
    UNIT_M_TO_MHZ,
    UNIT_C_TO_F,
    UNIT_F_TO_C,
    UNIT_COUNT,
} unit_conv_t;

char const *unit_conv_name(unit_conv_t c); // "dBm to mW"
char const *unit_in_name(unit_conv_t c);   // the unit of the number typed in: "dBm"

// A plain decimal number: optional sign, digits, optional ".digits" (a comma also works as the point). Spaces around it
// are ignored. false for anything else (letters, "1e5", "inf", an empty string).
bool unit_parse_number(char const *text, double *out);

typedef enum { UNIT_OK = 0, UNIT_NEEDS_POSITIVE, UNIT_OUT_OF_RANGE } unit_err_t;

// Converts in and writes the result with its unit ("25.12 mW", "-9.0 dBm", "0.3454 m", "68.0 °F"). A power, frequency
// or length must be above zero; inputs beyond +-1e9 are refused so the text always fits (out of at least 40 bytes).
unit_err_t unit_convert(unit_conv_t c, double in, char *out, size_t n);
char const *unit_err_text(unit_err_t e, unit_conv_t c); // "A power must be more than 0 mW"
