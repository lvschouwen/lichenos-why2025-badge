// Host test (#289): the console's on-air allow list. The same cases as tools/tests/guard_onair_test.sh where they
// apply on the badge (a listed channel or contact passes, any other target is refused, a target-less advert passes),
// plus the setting (off = no limit), the origins that may change it, the list's limits and its saved text.
// Made-up targets only: no real channel names or keys.
#include "mesh_onair.h"
#include "mutate.h"

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

static uint8_t const LISTED[ONAIR_PREFIX]   = {0xab, 0xcd, 0xef, 0x12, 0x34, 0x56};
static uint8_t const UNLISTED[ONAIR_PREFIX] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab};
// Made-up channel key ids (the first bytes of SHA-256 of a channel's secret on the badge)
static uint8_t const KEY_A[ONAIR_KEY_ID] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
static uint8_t const KEY_B[ONAIR_KEY_ID] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x81};

// Adds typed text as "fw onair allow" does: a channel bound to the key id its name resolves to (key)
static onair_result_t add(onair_list_t *l, char const *text, uint8_t const *key) {
    onair_entry_t  e;
    onair_result_t r = onair_entry_parse(text, &e);
    if (r != ONAIR_OK) {
        return r;
    }
    if (e.kind == ONAIR_CHANNEL) {
        memcpy(e.key_id, key, ONAIR_KEY_ID);
    }
    return onair_add(l, &e);
}

// A list with the test channel (key A) and the test node
static void bench_list(onair_list_t *l) {
    memset(l, 0, sizeof(*l));
    CHECK(add(l, "#zz-test", KEY_A) == ONAIR_OK);
    CHECK(add(l, "ABCDEF123456", NULL) == ONAIR_OK);
}

// A channel send passes key A (the listed channel's key) unless a test gives another
static bool allows(onair_list_t const *l, bool on, bool console, onair_tx_t tx, char const *ch, uint8_t const *p) {
    return mesh_onair_allows(l, on, console, tx, ch, KEY_A, p);
}

// The review of #289: a listed name re-created or renamed with another key is not the listed channel
static void test_channel_key(void) {
    onair_list_t l;
    bench_list(&l);
    CHECK(mesh_onair_allows(&l, true, true, ONAIR_TX_CHANNEL, "#zz-test", KEY_A, NULL));
    CHECK(!mesh_onair_allows(&l, true, true, ONAIR_TX_CHANNEL, "#zz-test", KEY_B, NULL));
    CHECK(!mesh_onair_allows(&l, true, true, ONAIR_TX_CHANNEL, "#ZZ-TEST", KEY_B, NULL));
    CHECK(!mesh_onair_allows(&l, true, true, ONAIR_TX_CHANNEL, "#zz-test", NULL, NULL)); // no channel resolved
    CHECK(!mesh_onair_allows(&l, true, true, ONAIR_TX_CHANNEL, "#zz-other", KEY_A, NULL)); // the key under another name
    CHECK(mesh_onair_allows(&l, false, true, ONAIR_TX_CHANNEL, "#zz-test", KEY_B, NULL));  // off: as before
    CHECK(mesh_onair_allows(&l, true, false, ONAIR_TX_CHANNEL, "#zz-test", KEY_B, NULL));  // a screen's
    // Allowing the name again after the channel changed: the entry takes the new key, the old one no longer passes
    CHECK(add(&l, "#zz-test", KEY_A) == ONAIR_DUPLICATE);
    CHECK(add(&l, "#ZZ-Test", KEY_B) == ONAIR_REKEYED && l.n == 2);
    CHECK(mesh_onair_allows(&l, true, true, ONAIR_TX_CHANNEL, "#zz-test", KEY_B, NULL));
    CHECK(!mesh_onair_allows(&l, true, true, ONAIR_TX_CHANNEL, "#zz-test", KEY_A, NULL));
    // Removed by name, whatever its key
    CHECK(onair_remove(&l, "#zz-test") == ONAIR_OK && l.n == 1);
    CHECK(!mesh_onair_allows(&l, true, true, ONAIR_TX_CHANNEL, "#zz-test", KEY_B, NULL));
}

static void test_decision(void) {
    onair_list_t l;
    bench_list(&l);
    // On, from the console: only the listed targets
    CHECK(allows(&l, true, true, ONAIR_TX_CHANNEL, "#zz-test", NULL));
    CHECK(allows(&l, true, true, ONAIR_TX_CHANNEL, "#ZZ-Test", NULL)); // the badge's channel names: ASCII case
    CHECK(!allows(&l, true, true, ONAIR_TX_CHANNEL, "#zz-other", NULL));
    CHECK(!allows(&l, true, true, ONAIR_TX_CHANNEL, "zz-test", NULL));
    CHECK(!allows(&l, true, true, ONAIR_TX_CHANNEL, "#zz-test2", NULL));
    CHECK(allows(&l, true, true, ONAIR_TX_CONTACT, NULL, LISTED));
    CHECK(!allows(&l, true, true, ONAIR_TX_CONTACT, NULL, UNLISTED));
    // Unknown target: none given, or of the other kind
    CHECK(!allows(&l, true, true, ONAIR_TX_CHANNEL, NULL, NULL));
    CHECK(!allows(&l, true, true, ONAIR_TX_CHANNEL, "", NULL));
    CHECK(!allows(&l, true, true, ONAIR_TX_CONTACT, NULL, NULL));
    CHECK(!allows(&l, true, true, ONAIR_TX_CHANNEL, "abcdef123456", LISTED));
    CHECK(!allows(&l, true, true, ONAIR_TX_CONTACT, "#zz-test", UNLISTED));
    CHECK(!allows(NULL, true, true, ONAIR_TX_CHANNEL, "#zz-test", NULL));
    // The advert has no target: it passes (owner decision 2026-11-13)
    CHECK(allows(&l, true, true, ONAIR_TX_ADVERT, NULL, NULL));
    // The setting off: today's behaviour, nothing limited
    CHECK(allows(&l, false, true, ONAIR_TX_CHANNEL, "#zz-other", NULL));
    CHECK(allows(&l, false, true, ONAIR_TX_CONTACT, NULL, UNLISTED));
    CHECK(allows(NULL, false, true, ONAIR_TX_CONTACT, NULL, NULL));
    // A screen's send is never limited
    CHECK(allows(&l, true, false, ONAIR_TX_CHANNEL, "#zz-other", NULL));
    CHECK(allows(&l, true, false, ONAIR_TX_CONTACT, NULL, UNLISTED));
    // On with an empty list: every console send with a target is refused
    onair_list_t empty = {0};
    CHECK(!allows(&empty, true, true, ONAIR_TX_CHANNEL, "#zz-test", NULL));
    CHECK(!allows(&empty, true, true, ONAIR_TX_CONTACT, NULL, LISTED));
    CHECK(allows(&empty, true, true, ONAIR_TX_ADVERT, NULL, NULL));
}

static void test_entries(void) {
    onair_list_t  l = {0};
    onair_entry_t e;
    char          text[64];
    // Key digits: 12 to 64, even, matched by their first 6 bytes; mistyped digits are refused, never a channel
    CHECK(onair_entry_parse("  abcdef123456  ", &e) == ONAIR_OK && e.kind == ONAIR_CONTACT);
    CHECK(memcmp(e.prefix, LISTED, ONAIR_PREFIX) == 0);
    CHECK(onair_entry_parse("abcdef1234567890", &e) == ONAIR_OK && memcmp(e.prefix, LISTED, ONAIR_PREFIX) == 0);
    CHECK(onair_entry_parse("abcdef12345", &e) == ONAIR_BAD);   // 11
    CHECK(onair_entry_parse("abcdef1234567", &e) == ONAIR_BAD); // odd
    char long_hex[ONAIR_HEX_MAX + 3];
    memset(long_hex, 'a', ONAIR_HEX_MAX);
    long_hex[ONAIR_HEX_MAX] = '\0';
    CHECK(onair_entry_parse(long_hex, &e) == ONAIR_OK);
    long_hex[ONAIR_HEX_MAX] = 'a', long_hex[ONAIR_HEX_MAX + 1] = 'a', long_hex[ONAIR_HEX_MAX + 2] = '\0';
    CHECK(onair_entry_parse(long_hex, &e) == ONAIR_BAD);
    CHECK(onair_entry_parse("cafe", &e) == ONAIR_BAD);
    // Channel names: 1 to ONAIR_NAME_MAX bytes, no control characters
    CHECK(onair_entry_parse("#zz-test", &e) == ONAIR_OK && e.kind == ONAIR_CHANNEL && !strcmp(e.name, "#zz-test"));
    CHECK(onair_entry_parse("zz private", &e) == ONAIR_OK && !strcmp(e.name, "zz private"));
    CHECK(onair_entry_parse("", &e) == ONAIR_BAD);
    CHECK(onair_entry_parse("   ", &e) == ONAIR_BAD);
    CHECK(onair_entry_parse(NULL, &e) == ONAIR_BAD);
    CHECK(onair_entry_parse("#zz\ttest", &e) == ONAIR_BAD);
    CHECK(onair_entry_parse("#zz\ntest", &e) == ONAIR_BAD);
    CHECK(onair_entry_parse("#zz\x7ftest", &e) == ONAIR_BAD);
    CHECK(onair_entry_parse("#123456789012345678901234567890", &e) == ONAIR_OK);   // 31
    CHECK(onair_entry_parse("#1234567890123456789012345678901", &e) == ONAIR_BAD); // 32
    // Duplicates (any case, any key length), full, remove
    CHECK(add(&l, "#zz-test", KEY_A) == ONAIR_OK);
    CHECK(add(&l, "#ZZ-TEST", KEY_A) == ONAIR_DUPLICATE);
    CHECK(add(&l, "abcdef123456", NULL) == ONAIR_OK);
    CHECK(add(&l, "ABCDEF123456ffff", NULL) == ONAIR_DUPLICATE);
    CHECK(add(&l, "bad\x01", KEY_A) == ONAIR_BAD && l.n == 2);
    for (int i = 2; i < ONAIR_MAX; i++) {
        snprintf(text, sizeof(text), "#zz-%d", i);
        CHECK(add(&l, text, KEY_B) == ONAIR_OK);
    }
    CHECK(l.n == ONAIR_MAX);
    CHECK(add(&l, "#zz-one-more", KEY_A) == ONAIR_FULL && l.n == ONAIR_MAX);
    CHECK(add(&l, "#zz-test", KEY_A) == ONAIR_DUPLICATE); // said before full: it is on the list
    CHECK(onair_remove(&l, "#zz-nothere") == ONAIR_NOT_FOUND);
    CHECK(onair_remove(&l, "") == ONAIR_BAD);
    CHECK(onair_remove(&l, "#ZZ-TEST") == ONAIR_OK && l.n == ONAIR_MAX - 1);
    CHECK(!mesh_onair_allows(&l, true, true, ONAIR_TX_CHANNEL, "#zz-test", KEY_A, NULL));
    CHECK(mesh_onair_allows(&l, true, true, ONAIR_TX_CONTACT, NULL, NULL, LISTED)); // the rest moved up, still there
    CHECK(onair_remove(&l, "abcdef123456") == ONAIR_OK);
    CHECK(!mesh_onair_allows(&l, true, true, ONAIR_TX_CONTACT, NULL, NULL, LISTED));
    CHECK(add(&l, "#zz-one-more", KEY_A) == ONAIR_OK);
    onair_entry_text(&l.e[l.n - 1], text, sizeof(text));
    CHECK(!strcmp(text, "channel #zz-one-more"));
    onair_entry_parse("ABCDEF123456", &e);
    onair_entry_text(&e, text, sizeof(text));
    CHECK(!strcmp(text, "contact abcdef123456"));
}

static void test_refused_text(void) {
    char out[ONAIR_REFUSED_MAX];
    onair_refused_text(ONAIR_TX_CHANNEL, "#zz-other", NULL, out, sizeof(out));
    CHECK(!strcmp(out, "on air refused: #zz-other is not on the console allow list"));
    onair_refused_text(ONAIR_TX_CONTACT, NULL, UNLISTED, out, sizeof(out));
    CHECK(!strcmp(out, "on air refused: 0123456789ab is not on the console allow list"));
    onair_refused_text(ONAIR_TX_CONTACT, NULL, NULL, out, sizeof(out));
    CHECK(!strcmp(out, "on air refused: ? is not on the console allow list"));
    onair_refused_text(ONAIR_TX_CHANNEL, "#123456789012345678901234567890", NULL, out, sizeof(out));
    CHECK(strlen(out) < sizeof(out) && strstr(out, "#123456789012345678901234567890 is not"));
    onair_refused_text(ONAIR_TX_CHANNEL, "#zz", NULL, out, 10); // cut, terminated
    CHECK(strlen(out) == 9);
}

static void test_ops(void) {
    char const *rest;
    CHECK(onair_op_parse("", &rest) == ONAIR_OP_STATUS);
    CHECK(onair_op_parse(NULL, &rest) == ONAIR_OP_STATUS);
    CHECK(onair_op_parse("on", &rest) == ONAIR_OP_ON);
    CHECK(onair_op_parse("off", &rest) == ONAIR_OP_OFF);
    CHECK(onair_op_parse("clear", &rest) == ONAIR_OP_CLEAR);
    CHECK(onair_op_parse("allow #zz-test", &rest) == ONAIR_OP_ALLOW && !strcmp(rest, "#zz-test"));
    CHECK(onair_op_parse("remove abcdef123456", &rest) == ONAIR_OP_REMOVE && !strcmp(rest, "abcdef123456"));
    CHECK(onair_op_parse("allow", &rest) == ONAIR_OP_USAGE);
    CHECK(onair_op_parse("allow ", &rest) == ONAIR_OP_USAGE);
    CHECK(onair_op_parse("off now", &rest) == ONAIR_OP_USAGE);
    CHECK(onair_op_parse("offline", &rest) == ONAIR_OP_USAGE);
    CHECK(onair_op_parse("onx", &rest) == ONAIR_OP_USAGE);
    CHECK(onair_op_parse("OFF", &rest) == ONAIR_OP_USAGE);
    // Origins: "fw onair"'s rows in mesh_cmd_table.c, pinned in console_pin_test.c (#464)
}

static void test_saved_text(void) {
    onair_list_t l, back;
    char         text[ONAIR_TEXT_MAX];
    bench_list(&l);
    CHECK(onair_encode(&l, text, sizeof(text)));
    CHECK(!strcmp(text, "v2\nc 1020304050607080 #zz-test\nk abcdef123456"));
    CHECK(onair_decode(text, &back) && back.n == 2 && !memcmp(&back, &l, sizeof(l)));
    CHECK(!onair_encode(&l, text, 10) && text[0] == '\0');
    CHECK(onair_decode("", &back) && back.n == 0);
    CHECK(onair_decode(NULL, &back) && back.n == 0);
    CHECK(onair_decode("v2", &back) && back.n == 0);
    memset(&l, 0, sizeof(l));
    CHECK(onair_encode(&l, text, sizeof(text)) && text[0] == '\0'); // empty: erased, not saved
    // A full list of the longest names fits the text
    memset(&l, 0, sizeof(l));
    for (int i = 0; i < ONAIR_MAX; i++) {
        char name[ONAIR_NAME_MAX + 1];
        snprintf(name, sizeof(name), "#%029d%d", 0, i);
        CHECK(add(&l, name, KEY_A) == ONAIR_OK);
    }
    CHECK(onair_encode(&l, text, sizeof(text)) && onair_decode(text, &back) && back.n == ONAIR_MAX);
    // Strict: damaged, repeated, too many: false and an empty list
#define K "1020304050607080 "
    static char const *const bad[] = {
        // the first format (efdb68b7, no version, no key id) and other versions: none
        "c #zz-test", "k abcdef123456", "c #zz-test\nk abcdef123456", "v1\nk abcdef123456", "v3\nk abcdef123456",
        "v2x\nk abcdef123456", "V2\nk abcdef123456", "v2\n", "\nv2",
        // damaged lines
        "v2\nx #zz", "v2\nc", "v2\nc ", "v2\nc " K, "v2\nc  " K "zz", "v2\nc #zz", "v2\nc 102030405060708 #zz",
        "v2\nc 10203040506070809 #zz", "v2\nc 102030405060708g #zz", "v2\nc " K " zz", "v2\nk abcdef12345",
        "v2\nk abcdef1234567", "v2\nk abcdef12345g", "v2\nc " K "#zz\n", "v2\n\nc " K "#zz", "v2\nc\t" K "#zz",
        "v2\nc " K "#zz\n\nc " K "#yy", "v2\nv2\nc " K "#zz",
        // repeated (a name with another key too) and too many
        "v2\nc " K "#zz\nc " K "#ZZ", "v2\nc " K "#zz\nc 0000000000000000 #zz", "v2\nk abcdef123456\nk ABCDEF123456",
        "v2\nc " K "#a\nc " K "#b\nc " K "#c\nc " K "#d\nc " K "#e\nc " K "#f\nc " K "#g\nc " K "#h\nc " K "#i",
    };
#undef K
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        back.n = 1;
        CHECK(!onair_decode(bad[i], &back) && back.n == 0);
        if (back.n) {
            printf("  decoded: \"%s\"\n", bad[i]);
        }
    }
    char huge[ONAIR_TEXT_MAX + 8];
    memset(huge, 'c', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    CHECK(!onair_decode(huge, &back) && back.n == 0);
}

// Mutated saved text never reads out of bounds and gives either false and none or a list that encodes back unchanged
static void test_mutated(void) {
    onair_list_t l, back;
    char         text[ONAIR_TEXT_MAX];
    uint8_t      buf[ONAIR_TEXT_MAX + 64];
    char         again[ONAIR_TEXT_MAX];
    bench_list(&l);
    CHECK(add(&l, "#zz private", KEY_B) == ONAIR_OK);
    CHECK(onair_encode(&l, text, sizeof(text)));
    mut_rng_t rng;
    mut_seed(&rng, 289);
    for (int i = 0; i < 4000; i++) {
        size_t   n = mut_random(&rng, (uint8_t const *)text, strlen(text), buf, sizeof(buf) - 1);
        uint8_t *exact = mut_exact(buf, n + 1);
        exact[n]       = '\0';
        bool ok        = onair_decode((char const *)exact, &back);
        if (ok) {
            CHECK(back.n <= ONAIR_MAX);
            CHECK(onair_encode(&back, again, sizeof(again)));
        } else {
            CHECK(back.n == 0);
        }
        free(exact);
    }
}

int main(void) {
    test_decision();
    test_channel_key();
    test_entries();
    test_refused_text();
    test_ops();
    test_saved_text();
    test_mutated();
    if (failures) {
        printf("mesh_onair_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_onair_test: ok\n");
    return 0;
}
