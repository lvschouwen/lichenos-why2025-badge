// The update token's text rules (#447), pinned from the code before the server's split: 32 lowercase hex characters,
// anything after them ignored (the file reader cuts there; the NVS reader checks the length itself)
#include "ota_proto.h"
#include "ota_token.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define TOKEN "0123456789abcdef0123456789abcdef"

static void test_text_valid(void) {
    CHECK(OTA_TOKEN_LEN == 32);
    CHECK(ota_token_text_valid(TOKEN));
    CHECK(ota_token_text_valid("ffffffffffffffffffffffffffffffff"));
    CHECK(ota_token_text_valid("00000000000000000000000000000000"));
    CHECK(ota_token_text_valid(TOKEN "\n"));     // the file's newline, and what follows, is not looked at
    CHECK(ota_token_text_valid(TOKEN "zz"));
    CHECK(!ota_token_text_valid("0123456789ABCDEF0123456789abcdef")); // uppercase: not what the badge writes
    CHECK(!ota_token_text_valid("0123456789abcdef0123456789abcdeg"));
    CHECK(!ota_token_text_valid("0123456789abcdef0123456789abcde"));   // one short: the NUL is not hex
    CHECK(!ota_token_text_valid(""));
    CHECK(!ota_token_text_valid(" 123456789abcdef0123456789abcdef"));
    CHECK(!ota_token_text_valid("0123456789abcdef\n123456789abcdef"));
    // Every byte that is not 0-9 or a-f, in every position
    char text[OTA_TOKEN_LEN + 1];
    for (int pos = 0; pos < OTA_TOKEN_LEN; pos++) {
        for (int c = 1; c < 256; c++) {
            memcpy(text, TOKEN, sizeof(text));
            text[pos] = (char)c;
            bool hex  = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            CHECK(ota_token_text_valid(text) == hex);
        }
    }
}

int main(void) {
    test_text_valid();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ota_token_test: all passed\n");
    return 0;
}
