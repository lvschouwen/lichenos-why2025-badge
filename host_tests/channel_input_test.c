// Host test: what the console and the screen accept as a new channel (#67; the #65 review's key overrun)
#include "channel_input.h"

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

static char    name[32];
static uint8_t secret[CHANNEL_SECRET_MAX + 8]; // the tail must stay untouched
static size_t  len;

static char const *parse(char const *n, char const *key) {
    memset(secret, 0xEE, sizeof(secret));
    len = 99;
    return channel_parse(n, key, name, sizeof(name), secret, &len);
}

static int tail_untouched(void) {
    for (size_t i = CHANNEL_SECRET_MAX; i < sizeof(secret); i++) {
        if (secret[i] != 0xEE) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    // 32 bytes: 0x00..0x1F; 16 bytes: 0x00..0x0F
    char const *key32 = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
    char const *key16 = "AAECAwQFBgcICQoLDA0ODw==";
    CHECK(strlen(key32) == 44 && strlen(key16) == 24);

    CHECK(parse("friends", key32) == NULL && len == 32 && secret[0] == 0 && secret[31] == 31 && tail_untouched());
    CHECK(strcmp(name, "friends") == 0);
    CHECK(parse("  Friends ", key16) == NULL && len == 16 && secret[15] == 15 && strcmp(name, "Friends") == 0);
    CHECK(parse("x", "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8") == NULL && len == 32); // unpadded
    CHECK(parse("x", "AAECAwQFBgcICQoLDA0ODw") == NULL && len == 16);

    // Too long, trailing junk, wrong sizes, bad characters: refused with a reason, nothing past the secret written
    CHECK(parse("x", "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=A") != NULL && tail_untouched()); // 45 chars
    CHECK(parse("x", "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMA==") != NULL &&
          tail_untouched());
    CHECK(parse("x", "AAECAwQFBgcICQoLDA0ODw==junk") != NULL);
    CHECK(parse("x", "AAECAwQFBgcICQoLDA0ODw== ") != NULL);
    CHECK(parse("x", "AAECAwQFBgcICQoLDA0O") != NULL);        // 15 bytes
    CHECK(parse("x", "AAECAwQFBgcICQoLDA0ODw=A") != NULL);    // padding in the middle
    CHECK(parse("x", "AAECAwQFBgcICQoLDA0OD!==") != NULL);    // not base64
    CHECK(parse("x", "AAECAwQFBgcICQoLDA0ODw===") != NULL);   // too much padding

    // Hashtags
    CHECK(parse("#Foo", NULL) == NULL && strcmp(name, "#foo") == 0 && len == 0);
    CHECK(parse(" #nederland ", "") == NULL && strcmp(name, "#nederland") == 0 && len == 0);
    CHECK(parse("#a b", NULL) != NULL);
    CHECK(parse("#", NULL) != NULL);
    CHECK(parse("foo", NULL) != NULL); // no key and no #
    CHECK(parse("   ", key16) != NULL);
    CHECK(parse("#0123456789012345678901234567890", NULL) != NULL); // 32 bytes
    CHECK(parse("#012345678901234567890123456789", NULL) == NULL);  // 31 bytes

    // Duplicates, for both kinds: without case
    CHECK(channel_same_name("#foo", "#Foo"));
    CHECK(channel_same_name("Public", "public"));
    CHECK(!channel_same_name("#foo", "#foo2"));
    CHECK(!channel_same_name("#foo", "foo"));

    // Which slots can be removed: not Public (slot 0), not an empty or missing slot
    CHECK(channel_removable(0, "Public") != NULL);
    CHECK(channel_removable(1, "#a") == NULL);
    CHECK(channel_removable(7, "x") == NULL);
    CHECK(channel_removable(1, "") != NULL);
    CHECK(channel_removable(1, NULL) != NULL);
    CHECK(channel_removable(-1, "#a") != NULL);
    CHECK(channel_removable(CHANNEL_SLOTS, "#a") != NULL);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("channel_input_test: all checks passed\n");
    return 0;
}
