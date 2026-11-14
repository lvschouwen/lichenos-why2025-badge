// The Wi-Fi service's bounded copy (#457): cap 0 writes nothing (it used to underflow cap - 1), a NULL source copies as
// an empty text, and valid input is cut at cap - 1 bytes or a NUL, also for an unterminated driver SSID field.
#include "wifi_text.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void cap_zero_writes_nothing(void) {
    char buf[4] = {'x', 'y', 'z', 'w'};
    wifi_text_copy(buf, 0, "abc"); // used to read and write far past the buffer
    CHECK(memcmp(buf, "xyzw", 4) == 0);
    wifi_text_copy(buf, 0, NULL);
    CHECK(memcmp(buf, "xyzw", 4) == 0);
    wifi_text_copy(NULL, 4, "abc");
    wifi_text_copy(NULL, 0, NULL);
}

static void null_source_is_empty(void) {
    char buf[4] = {'x', 'y', 'z', 'w'};
    wifi_text_copy(buf, sizeof(buf), NULL);
    CHECK(buf[0] == '\0');
    char one[1] = {'x'};
    wifi_text_copy(one, 1, NULL);
    CHECK(one[0] == '\0');
}

static void valid_input_is_unchanged(void) {
    char buf[8];
    wifi_text_copy(buf, sizeof(buf), "abc");
    CHECK(strcmp(buf, "abc") == 0);
    wifi_text_copy(buf, sizeof(buf), "abcdefghij"); // cut at cap - 1
    CHECK(strcmp(buf, "abcdefg") == 0);
    wifi_text_copy(buf, sizeof(buf), "");
    CHECK(buf[0] == '\0');
    char one[1] = {'x'};
    wifi_text_copy(one, 1, "abc");
    CHECK(one[0] == '\0');
    char field[4] = {'a', 'b', 'c', 'd'}; // the driver's SSID field: no NUL
    wifi_text_copy(buf, 4, field); // reads at most cap - 1 bytes: never past the field
    CHECK(strcmp(buf, "abc") == 0);
}

int main(void) {
    cap_zero_writes_nothing();
    null_source_is_empty();
    valid_input_is_unchanged();
    if (fails) {
        printf("wifi_text_test: %d failed\n", fails);
        return 1;
    }
    printf("wifi_text_test: ok\n");
    return 0;
}
