// Hex text: lichen_hex.h. Pure.
#include "lichen_hex.h"

int lichen_hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool lichen_hex_decode(char const *text, size_t len, uint8_t *out, size_t bytes) {
    if (bytes > len / 2 || len != 2 * bytes) { // the first test keeps 2 * bytes from wrapping
        return false;
    }
    for (size_t i = 0; i < bytes; i++) { // all checked before the first write: a bad text leaves out as it was
        if (lichen_hex_digit(text[2 * i]) < 0 || lichen_hex_digit(text[2 * i + 1]) < 0) {
            return false;
        }
    }
    for (size_t i = 0; i < bytes; i++) {
        out[i] = (uint8_t)(lichen_hex_digit(text[2 * i]) << 4 | lichen_hex_digit(text[2 * i + 1]));
    }
    return true;
}

bool lichen_hex_encode(uint8_t const *bytes, size_t n, char *out, size_t cap) {
    static char const digits[] = "0123456789abcdef";
    if (cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (n > (cap - 1) / 2) { // 2 * n + 1 > cap, written so a huge n cannot wrap
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * n] = '\0';
    return true;
}
