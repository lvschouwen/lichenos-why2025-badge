// Host test: which key a channel name may carry (#451). A #name holds and sends only its hashtag's key; a name without
// '#' takes any key. Vectors: the first 16 bytes of SHA-256 of the lowercased name (Python's hashlib), for the public
// test channel and a dummy name; the other keys are dummies.
#include "channel_input.h"
#include "channel_key.h"
#include "mesh_hist_crypto.h"

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

static uint8_t const BADGETEST[16] = {
    0x88, 0xc9, 0xf1, 0xd7, 0x00, 0x80, 0x2e, 0xbd, 0x7a, 0xaa, 0x09, 0xb3, 0x23, 0x4d, 0x48, 0x71
};
static uint8_t const FOO[16] = {
    0x5b, 0xca, 0xdc, 0x2b, 0x60, 0xbe, 0x87, 0xd5, 0x2f, 0x21, 0x3e, 0x04, 0x11, 0xb5, 0x43, 0x8e
};
#define BADGETEST_B64 "iMnx1wCALr16qgmzI01IcQ=="
#define DUMMY16_B64   "AAECAwQFBgcICQoLDA0ODw=="                     // 0x00..0x0F
#define DUMMY32_B64   "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=" // 0x00..0x1F

// What mesh_channel_add decides: channel_parse, then a typed key checked against the name. NULL when added.
static char const *add(char const *name_in, char const *b64, char *name, size_t cap) {
    uint8_t     secret[CHANNEL_SECRET_MAX] = {0};
    size_t      len                        = 0;
    char const *why                        = channel_parse(name_in, b64, name, cap, secret, &len);
    if (!why && len) {
        why = channel_key_refusal(name, secret, len);
    }
    return why;
}

static void derivation(void) {
    uint8_t key[16];
    CHECK(channel_hashtag_key("#badgetest", key) && memcmp(key, BADGETEST, 16) == 0);
    CHECK(channel_hashtag_key("#BadgeTest", key) && memcmp(key, BADGETEST, 16) == 0); // lowercased as the apps do
    CHECK(channel_hashtag_key("#foo", key) && memcmp(key, FOO, 16) == 0);
    CHECK(!channel_hashtag_key("#0123456789012345678901234567890", key)); // 32 bytes: no channel name is that long
    CHECK(!channel_hashtag_key(NULL, key));
    mh_test_fail(true);
    memset(key, 0xEE, sizeof(key));
    CHECK(!channel_hashtag_key("#badgetest", key) && key[0] == 0 && key[15] == 0); // a failed hash gives no key
    mh_test_fail(false);
}

static void adds(void) {
    char name[32];
    // The hashtag's own key, typed: added, under the lowercased name, with case and spaces around it
    CHECK(add("#badgetest", BADGETEST_B64, name, sizeof(name)) == NULL && strcmp(name, "#badgetest") == 0);
    CHECK(add("  #BadgeTest ", BADGETEST_B64, name, sizeof(name)) == NULL && strcmp(name, "#badgetest") == 0);
    CHECK(add("#BADGETEST", "iMnx1wCALr16qgmzI01IcQ", name, sizeof(name)) == NULL); // unpadded
    // Without a key: the hashtag channel, as before
    CHECK(add("#badgetest", NULL, name, sizeof(name)) == NULL && strcmp(name, "#badgetest") == 0);
    // A foreign key under a #name: refused, 16 or 32 bytes, whatever the case of the name
    CHECK(add("#badgetest", DUMMY16_B64, name, sizeof(name)) == CHANNEL_WRONG_KEY);
    CHECK(add("#badgetest", DUMMY32_B64, name, sizeof(name)) == CHANNEL_WRONG_KEY);
    CHECK(add("#BadgeTest", DUMMY16_B64, name, sizeof(name)) == CHANNEL_WRONG_KEY);
    CHECK(add("#foo", BADGETEST_B64, name, sizeof(name)) == CHANNEL_WRONG_KEY); // another hashtag's key
    // A #name with a key follows the hashtag name rules too
    CHECK(add("#badge test", BADGETEST_B64, name, sizeof(name)) != NULL);
    CHECK(add("#", DUMMY16_B64, name, sizeof(name)) != NULL);
    // A name without '#': any key, as before
    CHECK(add("badgetest", DUMMY16_B64, name, sizeof(name)) == NULL && strcmp(name, "badgetest") == 0);
    CHECK(add("Team chat", DUMMY32_B64, name, sizeof(name)) == NULL && strcmp(name, "Team chat") == 0);
    CHECK(add("friends", BADGETEST_B64, name, sizeof(name)) == NULL);
    // The hash failing refuses a keyed #name, never a name without '#'
    mh_test_fail(true);
    CHECK(add("#badgetest", BADGETEST_B64, name, sizeof(name)) == CHANNEL_WRONG_KEY);
    CHECK(add("friends", DUMMY16_B64, name, sizeof(name)) == NULL);
    mh_test_fail(false);
}

// Stored channels: rename to a #name, a send, the list (MeshCore keeps 32 bytes, a 16-byte key zero-padded)
static void stored(void) {
    uint8_t secret[32] = {0};
    memcpy(secret, BADGETEST, 16);
    CHECK(channel_stored_key_refusal("#badgetest", secret) == NULL);
    CHECK(channel_stored_key_refusal("#BadgeTest", secret) == NULL); // stored by another firmware in another case
    CHECK(channel_stored_key_refusal("#foo", secret) == CHANNEL_WRONG_KEY);
    CHECK(channel_stored_key_refusal("badgetest", secret) == NULL);
    secret[31] = 1; // a 32-byte key starting with the hashtag's: not that channel (its hash differs)
    CHECK(channel_stored_key_refusal("#badgetest", secret) == CHANNEL_WRONG_KEY);
    uint8_t dummy[32];
    for (int i = 0; i < 32; i++) {
        dummy[i] = (uint8_t)i;
    }
    CHECK(channel_stored_key_refusal("#badgetest", dummy) == CHANNEL_WRONG_KEY);
    CHECK(channel_stored_key_refusal("Public", dummy) == NULL);
    // The key lengths a typed key can have
    CHECK(channel_key_refusal("#badgetest", BADGETEST, 16) == NULL);
    CHECK(channel_key_refusal("#badgetest", BADGETEST, 32) == CHANNEL_WRONG_KEY);
    CHECK(channel_key_refusal(NULL, BADGETEST, 16) == NULL);
}

// Rename (#54) and add share one decision: a #new name keeps the channel's key only when it is the new name's
static void renames(void) {
    char    out[32];
    bool    tag;
    uint8_t secret[32] = {0};
    memcpy(secret, BADGETEST, 16);
    CHECK(
        channel_rename_parse("#BadgeTest", out, sizeof(out), &tag) == NULL && tag &&
        channel_stored_key_refusal(out, secret) == NULL
    );
    CHECK(
        channel_rename_parse("#foo", out, sizeof(out), &tag) == NULL && tag &&
        channel_stored_key_refusal(out, secret) == CHANNEL_WRONG_KEY
    );
    CHECK(channel_rename_parse("Team chat", out, sizeof(out), &tag) == NULL && !tag);
}

int main(void) {
    derivation();
    adds();
    stored();
    renames();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("channel_key_test: all checks passed\n");
    return 0;
}
