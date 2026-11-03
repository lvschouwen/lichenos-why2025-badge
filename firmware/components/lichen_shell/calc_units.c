#include "calc_units.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LIGHT_M_PER_US 299.792458 // metres per microsecond: wavelength[m] = this / frequency[MHz]
#define LIMIT          1e9

static struct {
    char const *name;
    char const *in;
} const conv[UNIT_COUNT] = {
    [UNIT_DBM_TO_MW] = {"dBm to mW", "dBm"},
    [UNIT_MW_TO_DBM] = {"mW to dBm", "mW"},
    [UNIT_MHZ_TO_M]  = {"MHz to wavelength", "MHz"},
    [UNIT_M_TO_MHZ]  = {"Wavelength to MHz", "m"},
    [UNIT_C_TO_F]    = {"\xC2\xB0""C to \xC2\xB0""F", "\xC2\xB0""C"},
    [UNIT_F_TO_C]    = {"\xC2\xB0""F to \xC2\xB0""C", "\xC2\xB0""F"},
};

char const *unit_conv_name(unit_conv_t c) {
    return (unsigned)c < UNIT_COUNT ? conv[c].name : "";
}

char const *unit_in_name(unit_conv_t c) {
    return (unsigned)c < UNIT_COUNT ? conv[c].in : "";
}

bool unit_parse_number(char const *text, double *out) {
    char buf[40];
    size_t len = 0;
    while (*text == ' ') {
        text++;
    }
    for (; *text && *text != ' '; text++) {
        if (len + 1 >= sizeof(buf)) {
            return false;
        }
        buf[len++] = *text == ',' ? '.' : *text;
    }
    while (*text == ' ') {
        text++;
    }
    buf[len] = '\0';
    if (*text != '\0') { // a second word
        return false;
    }
    size_t i = 0;
    if (buf[i] == '-' || buf[i] == '+') {
        i++;
    }
    size_t int_digits = 0, frac_digits = 0;
    double value = 0, scale = 1;
    for (; buf[i] >= '0' && buf[i] <= '9'; i++, int_digits++) {
        value = value * 10 + (buf[i] - '0');
    }
    if (buf[i] == '.') {
        for (i++; buf[i] >= '0' && buf[i] <= '9'; i++, frac_digits++) {
            scale /= 10;
            value += (buf[i] - '0') * scale;
        }
    }
    if (buf[i] != '\0' || int_digits + frac_digits == 0) {
        return false;
    }
    *out = buf[0] == '-' ? -value : value;
    return true;
}

// About four significant digits, never an exponent
static void fmt_sig(char *out, size_t n, double v, char const *unit) {
    double a = fabs(v);
    int decimals = 0;
    if (a > 0) {
        decimals = 3 - (int)floor(log10(a));
        decimals = decimals < 0 ? 0 : decimals > 12 ? 12 : decimals;
    }
    snprintf(out, n, "%.*f %s", decimals, v, unit);
}

unit_err_t unit_convert(unit_conv_t c, double in, char *out, size_t n) {
    if (!(fabs(in) <= LIMIT)) {
        return UNIT_OUT_OF_RANGE;
    }
    switch (c) {
    case UNIT_DBM_TO_MW:
        if (fabs(in) > 300) {
            return UNIT_OUT_OF_RANGE;
        }
        fmt_sig(out, n, pow(10.0, in / 10.0), "mW");
        return UNIT_OK;
    case UNIT_MW_TO_DBM:
        if (in <= 0) {
            return UNIT_NEEDS_POSITIVE;
        }
        snprintf(out, n, "%.1f dBm", 10.0 * log10(in));
        return UNIT_OK;
    case UNIT_MHZ_TO_M:
        if (in <= 0) {
            return UNIT_NEEDS_POSITIVE;
        }
        fmt_sig(out, n, LIGHT_M_PER_US / in, "m");
        return UNIT_OK;
    case UNIT_M_TO_MHZ:
        if (in <= 0) {
            return UNIT_NEEDS_POSITIVE;
        }
        fmt_sig(out, n, LIGHT_M_PER_US / in, "MHz");
        return UNIT_OK;
    case UNIT_C_TO_F:
        snprintf(out, n, "%.1f \xC2\xB0""F", in * 9.0 / 5.0 + 32.0);
        return UNIT_OK;
    case UNIT_F_TO_C:
        snprintf(out, n, "%.1f \xC2\xB0""C", (in - 32.0) * 5.0 / 9.0);
        return UNIT_OK;
    case UNIT_COUNT:
        break;
    }
    return UNIT_OUT_OF_RANGE;
}

char const *unit_err_text(unit_err_t e, unit_conv_t c) {
    if (e == UNIT_NEEDS_POSITIVE) {
        switch (c) {
        case UNIT_MW_TO_DBM: return "A power must be more than 0 mW";
        case UNIT_MHZ_TO_M:  return "A frequency must be more than 0 MHz";
        default:             return "A length must be more than 0 m";
        }
    }
    return e == UNIT_OK ? "" : "That number is too big";
}
