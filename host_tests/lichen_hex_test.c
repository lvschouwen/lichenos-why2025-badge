// Host test: the one hex text helper (#445). Pins the edges the copies it replaced shared or differed in.
#include "lichen_hex.h"

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

static void digits(void) {
    CHECK(lichen_hex_digit('0') == 0 && lichen_hex_digit('9') == 9);
    CHECK(lichen_hex_digit('a') == 10 && lichen_hex_digit('f') == 15);
    CHECK(lichen_hex_digit('A') == 10 && lichen_hex_digit('F') == 15); // either case
    // Every other byte is not a digit: the neighbours of each range, NUL, space, high bytes (signed char included)
    CHECK(lichen_hex_digit('g') == -1 && lichen_hex_digit('G') == -1 && lichen_hex_digit('@') == -1);
    CHECK(lichen_hex_digit('/') == -1 && lichen_hex_digit(':') == -1 && lichen_hex_digit('`') == -1);
    CHECK(lichen_hex_digit('\0') == -1 && lichen_hex_digit(' ') == -1 && lichen_hex_digit('-') == -1);
    int all = 0;
    for (int c = 0; c < 256; c++) {
        all += lichen_hex_digit((char)c) >= 0;
    }
    CHECK(all == 22); // 10 digits + 6 + 6 letters, nothing from the |0x20 trick or a sign extension
}

static void decode(void) {
    uint8_t out[4];
    memset(out, 0xee, sizeof(out));
    CHECK(lichen_hex_decode("00ff10Ab", 8, out, 4) && out[0] == 0x00 && out[1] == 0xff && out[2] == 0x10 && out[3] == 0xab);
    // Not NUL-terminated at len: the text after it is not read
    memset(out, 0xee, sizeof(out));
    CHECK(lichen_hex_decode("a1b2zz", 4, out, 2) && out[0] == 0xa1 && out[1] == 0xb2 && out[2] == 0xee);
    // Wrong length: short, long, odd. Out stays as it was.
    memset(out, 0xee, sizeof(out));
    CHECK(!lichen_hex_decode("a1b2", 4, out, 3) && !lichen_hex_decode("a1b2c3", 6, out, 2));
    CHECK(!lichen_hex_decode("a1b", 3, out, 1) && !lichen_hex_decode("a1b", 3, out, 2));
    CHECK(!lichen_hex_decode("", 0, out, 1) && !lichen_hex_decode("a1", 2, out, 0));
    // Non-hex in the first or the last digit of the last byte: nothing written, not even the good bytes before it
    CHECK(!lichen_hex_decode("a1b2c3zz", 8, out, 4) && !lichen_hex_decode("a1b2c3dg", 8, out, 4));
    CHECK(!lichen_hex_decode("a1 2", 4, out, 2) && !lichen_hex_decode("a1\0\0", 4, out, 2));
    CHECK(out[0] == 0xee && out[1] == 0xee && out[2] == 0xee && out[3] == 0xee);
    // Nothing to decode
    CHECK(lichen_hex_decode("", 0, out, 0) && lichen_hex_decode(NULL, 0, out, 0));
    CHECK(!lichen_hex_decode("a1", (size_t)-1, out, (size_t)1 << 63)); // a length that would wrap 2 * bytes
}

static void encode(void) {
    char    out[16];
    uint8_t b[4] = {0x00, 0xff, 0x1a, 0xB2};
    CHECK(lichen_hex_encode(b, 4, out, sizeof(out)) && strcmp(out, "00ff1ab2") == 0); // lower case
    CHECK(lichen_hex_encode(b, 4, out, 9) && strcmp(out, "00ff1ab2") == 0);          // exactly 2n+1
    memset(out, 'x', sizeof(out));
    CHECK(!lichen_hex_encode(b, 4, out, 8) && out[0] == '\0'); // one short: terminated empty, never half
    memset(out, 'x', sizeof(out));
    CHECK(!lichen_hex_encode(b, 4, out, 1) && out[0] == '\0' && out[1] == 'x');
    memset(out, 'x', sizeof(out));
    CHECK(!lichen_hex_encode(b, 4, out, 0) && out[0] == 'x'); // cap 0: nothing written
    CHECK(lichen_hex_encode(b, 0, out, 1) && out[0] == '\0'); // no bytes: the empty text
    CHECK(!lichen_hex_encode(b, (size_t)-1, out, 16) && out[0] == '\0');
    CHECK(lichen_hex_encode(b, 1, out, 3) && strcmp(out, "00") == 0);
}

static void round_trip(void) {
    uint8_t b[8] = {1, 2, 3, 0x80, 0xfe, 0xff, 0, 7}, back[8];
    char    t[17];
    CHECK(lichen_hex_encode(b, 8, t, sizeof(t)) && lichen_hex_decode(t, 16, back, 8) && memcmp(b, back, 8) == 0);
}

int main(void) {
    digits();
    decode();
    encode();
    round_trip();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
