// nvs_mark.c (#116): which NVS path an OTA slot's image was built with, and whether both slots allow the burn
#include "nvs_mark.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static nvs_mark_kind_t parse(uint8_t flags, uint8_t version) {
    nvs_mark_t m = {.tag = NVS_MARK_TAG, .version = version, .flags = flags};
    uint8_t    b[sizeof(m)];
    memcpy(b, &m, sizeof(m));
    return nvs_mark_parse(true, b, sizeof(b));
}

static void marks_decode(void) {
    CHECK(sizeof(nvs_mark_t) == 12);
    CHECK(parse(0, NVS_MARK_VERSION) == NVS_MARK_PLAIN);
    CHECK(parse(NVS_MARK_F_ENCRYPT, NVS_MARK_VERSION) == NVS_MARK_ENCRYPT);
    CHECK(parse(NVS_MARK_F_ENCRYPT | NVS_MARK_F_BURN, NVS_MARK_VERSION) == NVS_MARK_ENCRYPT_BURN);
    CHECK(parse(NVS_MARK_F_BURN, NVS_MARK_VERSION) == NVS_MARK_PLAIN); // burn needs encrypt (Kconfig depends on)
    CHECK(parse(NVS_MARK_F_ENCRYPT | 0x80, NVS_MARK_VERSION + 1) == NVS_MARK_ENCRYPT); // later version, new flag
    CHECK(parse(NVS_MARK_F_ENCRYPT, 0) == NVS_MARK_NONE);
}

// An image from before the mark has ordinary rodata at the offset: never read as an encrypt image
static void no_mark_and_no_app(void) {
    uint8_t b[sizeof(nvs_mark_t)];
    memset(b, 0xFF, sizeof(b)); // erased flash
    CHECK(nvs_mark_parse(true, b, sizeof(b)) == NVS_MARK_NONE);
    memset(b, 0, sizeof(b));
    CHECK(nvs_mark_parse(true, b, sizeof(b)) == NVS_MARK_NONE);
    nvs_mark_t m = {.tag = NVS_MARK_TAG, .version = NVS_MARK_VERSION, .flags = NVS_MARK_F_ENCRYPT};
    memcpy(b, &m, sizeof(m));
    b[3] ^= 0x20; // one tag byte off
    CHECK(nvs_mark_parse(true, b, sizeof(b)) == NVS_MARK_NONE);
    memcpy(b, &m, sizeof(m));
    CHECK(nvs_mark_parse(true, b, sizeof(b) - 1) == NVS_MARK_NONE); // short read
    CHECK(nvs_mark_parse(true, NULL, 0) == NVS_MARK_NONE);          // read failed
    CHECK(nvs_mark_parse(false, b, sizeof(b)) == NVS_MARK_NO_APP);  // no app description: the mark means nothing
}

static void rollback_needs_both(void) {
    nvs_mark_kind_t const all[] = {NVS_MARK_NO_APP, NVS_MARK_NONE, NVS_MARK_PLAIN, NVS_MARK_ENCRYPT,
                                   NVS_MARK_ENCRYPT_BURN};
    for (unsigned i = 0; i < 5; i++) {
        for (unsigned j = 0; j < 5; j++) {
            bool want = nvs_mark_encrypts(all[i]) && nvs_mark_encrypts(all[j]);
            CHECK(nvs_marks_rollback_safe(all[i], all[j]) == want);
        }
        CHECK(strlen(nvs_mark_name(all[i])) > 0);
    }
    CHECK(nvs_marks_rollback_safe(NVS_MARK_ENCRYPT, NVS_MARK_ENCRYPT));
    CHECK(nvs_marks_rollback_safe(NVS_MARK_ENCRYPT, NVS_MARK_ENCRYPT_BURN));
    CHECK(!nvs_marks_rollback_safe(NVS_MARK_ENCRYPT, NVS_MARK_NONE));
    CHECK(!nvs_marks_rollback_safe(NVS_MARK_ENCRYPT, NVS_MARK_NO_APP)); // an empty slot: the next OTA lands there
    CHECK(!nvs_marks_rollback_safe(NVS_MARK_PLAIN, NVS_MARK_ENCRYPT));
}

int main(void) {
    marks_decode();
    no_mark_and_no_app();
    rollback_needs_both();
    if (fails) {
        printf("%d failure(s)\n", fails);
        return 1;
    }
    printf("All nvs_mark tests passed\n");
    return 0;
}
