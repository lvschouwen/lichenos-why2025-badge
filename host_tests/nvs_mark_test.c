// nvs_mark.c (#116): what an OTA slot's image was built with, and whether both slots allow the burn
#include "nvs_mark.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static nvs_mark_info_t parse(uint8_t flags, uint8_t version) {
    nvs_mark_t m = {.tag = NVS_MARK_TAG, .version = version, .flags = flags};
    uint8_t    b[sizeof(m)];
    memcpy(b, &m, sizeof(m));
    return nvs_mark_parse(true, b, sizeof(b));
}

#define G NVS_MARK_F_GUARD
#define E NVS_MARK_F_ENCRYPT
#define B NVS_MARK_F_BURN

static void marks_decode(void) {
    CHECK(sizeof(nvs_mark_t) == 12);
    nvs_mark_info_t m = parse(G, NVS_MARK_VERSION);
    CHECK(m.app && m.marked && m.guard && !m.encrypt && !m.burn);
    m = parse(E | G, NVS_MARK_VERSION);
    CHECK(m.encrypt && m.guard && !m.burn);
    m = parse(E | B | G, NVS_MARK_VERSION);
    CHECK(m.encrypt && m.burn && m.guard);
    m = parse(B | G, NVS_MARK_VERSION); // burn needs encrypt (Kconfig depends on)
    CHECK(!m.encrypt && !m.burn);
    m = parse(E, NVS_MARK_VERSION);
    CHECK(m.marked && m.encrypt && !m.guard);
    m = parse(E | G | 0x80, NVS_MARK_VERSION + 1); // later version, new flag
    CHECK(m.marked && m.encrypt && m.guard);
    m = parse(E | G, 0);
    CHECK(m.app && !m.marked && !m.guard);
}

// An image from before the mark has ordinary rodata at the offset: never read as guarded
static void no_mark_and_no_app(void) {
    uint8_t b[sizeof(nvs_mark_t)];
    memset(b, 0xFF, sizeof(b)); // erased flash
    CHECK(!nvs_mark_parse(true, b, sizeof(b)).marked);
    memset(b, 0, sizeof(b));
    CHECK(!nvs_mark_parse(true, b, sizeof(b)).marked);
    nvs_mark_t m = {.tag = NVS_MARK_TAG, .version = NVS_MARK_VERSION, .flags = E | G};
    memcpy(b, &m, sizeof(m));
    b[3] ^= 0x20; // one tag byte off
    CHECK(!nvs_mark_parse(true, b, sizeof(b)).guard);
    memcpy(b, &m, sizeof(m));
    CHECK(!nvs_mark_parse(true, b, sizeof(b) - 1).marked); // short read
    CHECK(!nvs_mark_parse(true, NULL, 0).marked);          // read failed
    nvs_mark_info_t none = nvs_mark_parse(false, b, sizeof(b));
    CHECK(!none.app && !none.marked && !none.guard); // no app description: the mark means nothing
}

static void verdicts(void) {
    nvs_mark_info_t enc = parse(E | G, 1), burn = parse(E | B | G, 1), plain = parse(G, 1);
    nvs_mark_info_t old_enc = parse(E, 1), unmarked = nvs_mark_parse(true, NULL, 0), empty = nvs_mark_parse(false, NULL, 0);
    CHECK(nvs_slots_verdict(enc, enc) == NVS_SLOTS_SAFE);
    CHECK(nvs_slots_verdict(burn, enc) == NVS_SLOTS_SAFE);
    CHECK(nvs_slots_verdict(enc, plain) == NVS_SLOTS_GUARDED);
    CHECK(nvs_slots_verdict(plain, plain) == NVS_SLOTS_GUARDED);
    CHECK(nvs_slots_verdict(enc, old_enc) == NVS_SLOTS_UNSAFE); // a mark without the guard bit
    CHECK(nvs_slots_verdict(enc, unmarked) == NVS_SLOTS_UNSAFE); // a pre-guard image
    CHECK(nvs_slots_verdict(unmarked, enc) == NVS_SLOTS_UNSAFE);
    CHECK(nvs_slots_verdict(enc, empty) == NVS_SLOTS_UNSAFE); // an empty slot: the next OTA lands there
}

static void descriptions(void) {
    char out[48];
    nvs_mark_describe(parse(E | B | G, 1), out, sizeof(out));
    CHECK(strcmp(out, "encrypt + burn command, guard") == 0);
    nvs_mark_describe(parse(G, 1), out, sizeof(out));
    CHECK(strcmp(out, "plain, guard") == 0);
    nvs_mark_describe(parse(E, 1), out, sizeof(out));
    CHECK(strcmp(out, "encrypt, NO guard") == 0);
    nvs_mark_describe(nvs_mark_parse(true, NULL, 0), out, sizeof(out));
    CHECK(strcmp(out, "no mark (built before it: unguarded)") == 0);
    nvs_mark_describe(nvs_mark_parse(false, NULL, 0), out, sizeof(out));
    CHECK(strcmp(out, "no app") == 0);
}

int main(void) {
    marks_decode();
    no_mark_and_no_app();
    verdicts();
    descriptions();
    if (fails) {
        printf("%d failure(s)\n", fails);
        return 1;
    }
    printf("All nvs_mark tests passed\n");
    return 0;
}
