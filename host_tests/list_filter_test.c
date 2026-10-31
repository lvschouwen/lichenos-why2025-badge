// Host test: type-to-search matching for the list screens (#32, #57)
#include "list_filter.h"

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

static bool name_word(char const *word, void *ctx) {
    return list_filter_text_has(ctx, word);
}

static bool match(char const *name, char const *q) {
    return list_filter_match(q, name_word, (void *)name);
}

int main(void) {
    char q[32] = "";
    CHECK(list_filter_edit(q, sizeof(q), 'j') && strcmp(q, "j") == 0);
    CHECK(list_filter_edit(q, sizeof(q), 'A') && strcmp(q, "jA") == 0);
    CHECK(list_filter_edit(q, sizeof(q), 8) && strcmp(q, "j") == 0);
    CHECK(list_filter_edit(q, sizeof(q), 127) && q[0] == '\0');
    CHECK(!list_filter_edit(q, sizeof(q), 8) && !list_filter_edit(q, sizeof(q), 127));
    CHECK(!list_filter_edit(q, sizeof(q), ' ') && q[0] == '\0'); // no leading space
    list_filter_edit(q, sizeof(q), 'a');
    CHECK(list_filter_edit(q, sizeof(q), ' ') && strcmp(q, "a ") == 0);
    CHECK(!list_filter_edit(q, sizeof(q), ' ')); // no double space
    uint32_t const ignored[] = {0, 9, 10, 13, 17, 18, 19, 20, 27, 31, 128, 0x20AC};
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++) {
        CHECK(!list_filter_edit(q, sizeof(q), ignored[i]));
    }
    CHECK(strcmp(q, "a ") == 0);

    char big[64] = "";
    for (int i = 0; i < 40; i++) {
        list_filter_edit(big, sizeof(big), 'x');
    }
    CHECK(strlen(big) == LIST_FILTER_MAX);
    char tiny[4] = "";
    for (int i = 0; i < 10; i++) {
        list_filter_edit(tiny, sizeof(tiny), 'x');
    }
    CHECK(strlen(tiny) == 3); // the terminator always fits

    CHECK(match("Jacob", ""));
    CHECK(match("Jacob", "   "));
    CHECK(match("Jacob", "JAC"));
    CHECK(match("Jacob", "cob"));
    CHECK(!match("Jacob", "xyz"));
    CHECK(match("NL-HHW-HUYGENHOEK-OBS", "obs"));
    CHECK(match("NL-HHW-HUYGENHOEK-OBS", "huy obs")); // every word
    CHECK(!match("NL-HHW-HUYGENHOEK-OBS", "huy jac"));
    CHECK(match("Jacob", " jac "));
    CHECK(!list_filter_text_has(NULL, "a"));

    CHECK(list_filter_is_hex("a1F") && !list_filter_is_hex("a1g") && !list_filter_is_hex(""));
    CHECK(list_filter_is_number("869.6") && list_filter_is_number("915") && !list_filter_is_number("9a") &&
          !list_filter_is_number(".") && !list_filter_is_number(""));
    CHECK(list_filter_text_starts("869.618", "869.6") && !list_filter_text_starts("869.618", "69"));
    CHECK(!list_filter_text_starts("8", "86"));

    uint8_t const key[] = {0xA1, 0x0b, 0xff};
    CHECK(list_filter_key_starts(key, 3, "a1"));
    CHECK(list_filter_key_starts(key, 3, "A10"));
    CHECK(list_filter_key_starts(key, 3, "a10bff"));
    CHECK(!list_filter_key_starts(key, 3, "a10bff0")); // longer than the key
    CHECK(!list_filter_key_starts(key, 3, "b1"));
    CHECK(!list_filter_key_starts(key, 3, "obs")); // not hex
    CHECK(!list_filter_key_starts(key, 3, ""));

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("list_filter_test ok\n");
    return 0;
}
