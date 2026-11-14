// Host test: the one bounded string copy (#445). Pins the edges of the copies it replaced.
#include "lichen_str.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    char d[8];
    // Fits whole, exactly (cap - 1 chars), one too long
    memset(d, 'x', sizeof(d));
    CHECK(!lichen_str_copy(d, sizeof(d), "abc") && strcmp(d, "abc") == 0);
    CHECK(!lichen_str_copy(d, sizeof(d), "1234567") && strcmp(d, "1234567") == 0);
    CHECK(lichen_str_copy(d, sizeof(d), "12345678") && strcmp(d, "1234567") == 0);
    CHECK(lichen_str_copy(d, sizeof(d), "a much longer text") && strcmp(d, "a much ") == 0); // always terminated
    // Empty and NULL are the empty text, not a cut
    memset(d, 'x', sizeof(d));
    CHECK(!lichen_str_copy(d, sizeof(d), "") && d[0] == '\0');
    memset(d, 'x', sizeof(d));
    CHECK(!lichen_str_copy(d, sizeof(d), NULL) && d[0] == '\0');
    CHECK(!lichen_str_copy_n(d, sizeof(d), NULL, 5) && d[0] == '\0');
    // cap 1: only the terminator; cap 0: nothing written, cut for any text
    memset(d, 'x', sizeof(d));
    CHECK(lichen_str_copy(d, 1, "a") && d[0] == '\0' && d[1] == 'x');
    CHECK(!lichen_str_copy(d, 1, "") && d[0] == '\0');
    memset(d, 'x', sizeof(d));
    CHECK(lichen_str_copy(d, 0, "a") && d[0] == 'x');
    CHECK(!lichen_str_copy(d, 0, "") && !lichen_str_copy(d, 0, NULL) && d[0] == 'x');
    // The first n chars: shorter than the text, longer than it, zero
    CHECK(!lichen_str_copy_n(d, sizeof(d), "abcdef", 3) && strcmp(d, "abc") == 0); // the rest is not "cut", it is out of n
    CHECK(!lichen_str_copy_n(d, sizeof(d), "ab", 5) && strcmp(d, "ab") == 0);       // stops at the NUL
    CHECK(!lichen_str_copy_n(d, sizeof(d), "abc", 0) && d[0] == '\0');
    CHECK(lichen_str_copy_n(d, 3, "abcdef", 5) && strcmp(d, "ab") == 0);            // 5 asked, 2 fit
    CHECK(!lichen_str_copy_n(d, 3, "abcdef", 2) && strcmp(d, "ab") == 0);           // n 2 fits whole
    // An unterminated field (the driver's 32-byte SSID): never read past n
    char field[4] = {'w', 'i', 'f', 'i'};
    CHECK(!lichen_str_copy_n(d, sizeof(d), field, sizeof(field)) && strcmp(d, "wifi") == 0);
    CHECK(lichen_str_copy_n(d, 3, field, sizeof(field)) && strcmp(d, "wi") == 0);
    // A NUL inside n ends the copy (a copy by memcpy would carry it on; no caller has one)
    CHECK(!lichen_str_copy_n(d, sizeof(d), "ab\0cd", 5) && strcmp(d, "ab") == 0);
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
