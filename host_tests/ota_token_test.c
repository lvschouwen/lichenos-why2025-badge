// The update token (#447). Its text rules, pinned from the code before the server's split: 32 lowercase hex characters,
// anything after them ignored (the file reader cuts there; the NVS reader checks the length itself). Since the split,
// where it is loaded from and what is written back (ota_token_load), with the file at OTA_TOKEN_FILE (a scratch path)
// and the settings through a fake ota_os binding.
#include "lichen_random.h"
#include "ota_os.h"
#include "ota_proto.h"
#include "ota_token.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

// The settings' secret store, faked: one key, and whether reads and writes work
static char nvs_value[64];
static bool nvs_has, nvs_up = true;
static int  nvs_writes;

static bool fake_secret_get(char const *key, char *out, size_t cap) {
    if (!nvs_up || !nvs_has || strcmp(key, "ota_token") != 0) {
        out[0] = '\0';
        return false;
    }
    snprintf(out, cap, "%s", nvs_value);
    return true;
}

static bool fake_secret_set(char const *key, char const *value) {
    if (!nvs_up || strcmp(key, "ota_token") != 0) {
        return false;
    }
    snprintf(nvs_value, sizeof(nvs_value), "%s", value);
    nvs_has = true;
    nvs_writes++;
    return true;
}

static uint8_t random_byte;

void lichen_random_fill(void *buf, size_t len) {
    memset(buf, random_byte, len);
}

static void write_file(char const *text) {
    FILE *f = fopen(OTA_TOKEN_FILE, "w");
    CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static bool file_is(char const *text) {
    char  got[80] = {0};
    FILE *f       = fopen(OTA_TOKEN_FILE, "r");
    if (!f) {
        return false;
    }
    size_t n = fread(got, 1, sizeof(got) - 1, f);
    fclose(f);
    return n == strlen(text) && strcmp(got, text) == 0;
}

static void reset(void) {
    unlink(OTA_TOKEN_FILE);
    nvs_has    = false;
    nvs_up     = true;
    nvs_writes = 0;
    ota_token_forget();
}

static void test_load(void) {
    ota_os_t const os = {.secret_get = fake_secret_get, .secret_set = fake_secret_set};
    ota_os_bind(&os);

    // The file wins; NVS gets its copy
    reset();
    write_file(TOKEN "\n");
    CHECK(ota_token_load() && strcmp(ota_token_get(), TOKEN) == 0);
    CHECK(nvs_has && strcmp(nvs_value, TOKEN) == 0 && nvs_writes == 1);
    CHECK(file_is(TOKEN "\n")); // left as it was

    // Both agree: nothing is written
    nvs_writes = 0;
    ota_token_forget();
    CHECK(ota_token_load() && strcmp(ota_token_get(), TOKEN) == 0 && nvs_writes == 0);

    // No file: the NVS copy, written back to the file
    reset();
    nvs_has = true;
    snprintf(nvs_value, sizeof(nvs_value), "%s", "ffffffffffffffffffffffffffffffff");
    CHECK(ota_token_load() && strcmp(ota_token_get(), "ffffffffffffffffffffffffffffffff") == 0);
    CHECK(file_is("ffffffffffffffffffffffffffffffff\n") && nvs_writes == 0);

    // A bad file and a bad NVS copy: a new one from lichen_random, saved to both
    reset();
    write_file("0123456789ABCDEF0123456789ABCDEF\n");
    nvs_has = true;
    snprintf(nvs_value, sizeof(nvs_value), "%s", TOKEN "0"); // too long for the NVS reader
    random_byte = 0x5a;
    CHECK(ota_token_load() && strcmp(ota_token_get(), "5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a") == 0);
    CHECK(file_is("5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a\n") && strcmp(nvs_value, ota_token_get()) == 0);

    // NVS down and the file written: still kept (#116)
    reset();
    nvs_up = false;
    CHECK(ota_token_load() && file_is("5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a\n"));

    // Forgotten: empty, which ota_token_equal never matches
    ota_token_forget();
    CHECK(ota_token_get()[0] == '\0' && !ota_token_equal(ota_token_get(), ""));

    reset();
    ota_os_bind(NULL);
}

int main(void) {
    test_text_valid();
    test_load();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ota_token_test: all passed\n");
    return 0;
}
