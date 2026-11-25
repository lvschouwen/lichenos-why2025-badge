// Host test: the SD app manifest parser and the API version check (#521)
#include "lichen_manifest.h"

#include "lichen_api_version.h"
#include "lichen_caps.h"

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

static lichen_manifest_err_t parse(char const *text, char const *dir, lichen_manifest_t *m, unsigned *line) {
    return lichen_manifest_parse(text, strlen(text), dir, m, line);
}

static void test_good(void) {
    lichen_manifest_t m;
    unsigned          line = 9;
    char const       *t    = "# a comment\nid=notes\nname=Quick Notes\n\napi=1.0\nentry=notes.elf\nicon=icon.bin\n"
                             "caps=SD, NET LORA_TX\ninternal_ram_kib=8\npsram_kib=64\nstack_kib=4\n";
    CHECK(parse(t, "notes", &m, &line) == LICHEN_MANIFEST_OK);
    CHECK(line == 0);
    CHECK(strcmp(m.id, "notes") == 0 && strcmp(m.name, "Quick Notes") == 0);
    CHECK(strcmp(m.entry, "notes.elf") == 0 && strcmp(m.icon, "icon.bin") == 0);
    CHECK(m.api_major == 1 && m.api_minor == 0);
    CHECK(m.caps == (CAP_SD | CAP_NET | CAP_LORA_TX));
    CHECK(m.internal_ram_kib == 8 && m.psram_kib == 64 && m.stack_kib == 4);
}

static void test_defaults_and_endings(void) {
    lichen_manifest_t m;
    // CRLF, no trailing newline, default entry, no icon, no caps
    CHECK(parse("id=a1\r\nname=A\r\napi=1.2", "a1", &m, NULL) == LICHEN_MANIFEST_OK);
    CHECK(strcmp(m.entry, "app.elf") == 0 && m.icon[0] == '\0' && m.caps == 0 && m.stack_kib == 0);
    CHECK(strcmp(m.name, "A") == 0 && m.api_minor == 2);
}

static void test_refused(void) {
    lichen_manifest_t m;
    unsigned          line;
    CHECK(parse("name=A\napi=1.0\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_MISSING);
    CHECK(parse("id=a\nname=A\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_MISSING);
    CHECK(parse("id=A\nname=A\napi=1.0\n", "A", &m, &line) == LICHEN_MANIFEST_ERR_ID && line == 1);
    CHECK(parse("id=c6\nname=A\napi=1.0\n", "c6", &m, &line) == LICHEN_MANIFEST_ERR_ID);
    CHECK(parse("id=a\nname=A\napi=1.0\n", "b", &m, &line) == LICHEN_MANIFEST_ERR_ID); // not the directory's name
    CHECK(parse("id=a\nid=a\nname=A\napi=1.0\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_DUPLICATE && line == 2);
    CHECK(parse("id=a\nname=A\napi=1.0\ncaps=SD\ncaps=NET\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_DUPLICATE);
    CHECK(parse("id=a\nname=A\napi=1.0\nstak_kib=4\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_KEY && line == 4);
    CHECK(parse("id=a\nname=A\napi=1.0\njunk\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_LINE);
    CHECK(parse("id=a\nname=A\napi=1.0\n=x\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_LINE);
    CHECK(parse("id=a\nname=A\napi=1\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_VERSION);
    CHECK(parse("id=a\nname=A\napi=1.x\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_VERSION);
    CHECK(parse("id=a\nname=A\napi=1.0.1\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_VERSION);
    CHECK(parse("id=a\nname=A\napi=1.0\ncaps=VIDEO SD\n", "a", &m, &line) == LICHEN_MANIFEST_OK && m.caps == (CAP_VIDEO | CAP_SD)); // #562
    CHECK(parse("id=a\nname=A\napi=1.0\ncaps=SD BOGUS\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_VALUE);
    CHECK(parse("id=a\nname=A\napi=1.0\ncaps=SYSTEM\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_VALUE);
    CHECK(parse("id=a\nname=A\napi=1.0\nstack_kib=70000\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_VALUE);
    CHECK(parse("id=a\nname=A\napi=1.0\npsram_kib=-1\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_VALUE);
    CHECK(parse("id=a\nname=A\napi=1.0\nentry=../x.elf\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_VALUE);
    CHECK(parse("id=a\nname=A\napi=1.0\nicon=d/i.bin\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_VALUE);
    CHECK(parse("id=a\nname=\napi=1.0\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_VALUE);
    CHECK(parse("id=a\nname=A\napi=1.0\n", NULL, &m, &line) == LICHEN_MANIFEST_ERR_ID);
    CHECK(lichen_manifest_parse(NULL, 0, "a", &m, NULL) == LICHEN_MANIFEST_ERR_ARG);
}

static void test_long_and_nul(void) {
    lichen_manifest_t m;
    unsigned          line;
    char              big[300] = "id=a\nname=";
    size_t            n        = strlen(big);
    memset(big + n, 'x', LICHEN_MANIFEST_LINE_MAX); // the line is far over the limit
    big[n + LICHEN_MANIFEST_LINE_MAX] = '\n';
    strcpy(big + n + LICHEN_MANIFEST_LINE_MAX + 1, "api=1.0\n");
    CHECK(lichen_manifest_parse(big, strlen(big), "a", &m, &line) == LICHEN_MANIFEST_ERR_LINE && line == 2);
    char const nul[] = "id=a\nname=A\0x\napi=1.0\n";
    CHECK(lichen_manifest_parse(nul, sizeof(nul) - 1, "a", &m, &line) == LICHEN_MANIFEST_ERR_LINE && line == 2);
    // a name over 31 characters
    CHECK(parse("id=a\nname=0123456789012345678901234567890123\napi=1.0\n", "a", &m, &line) == LICHEN_MANIFEST_ERR_VALUE);
    // a failed parse leaves *out alone
    lichen_manifest_t keep;
    memset(&keep, 7, sizeof(keep));
    CHECK(parse("id=a\n", "a", &keep, NULL) == LICHEN_MANIFEST_ERR_MISSING && keep.api_major == 0x0707);
}

static void test_compatible(void) {
    CHECK(lichen_api_compatible(LICHEN_API_VERSION_MAJOR, LICHEN_API_VERSION_MINOR));
    CHECK(lichen_api_compatible(LICHEN_API_VERSION_MAJOR, 0));
    CHECK(!lichen_api_compatible(LICHEN_API_VERSION_MAJOR, LICHEN_API_VERSION_MINOR + 1));
    CHECK(!lichen_api_compatible(LICHEN_API_VERSION_MAJOR + 1, 0));
    CHECK(!lichen_api_compatible(0, 0) || LICHEN_API_VERSION_MAJOR == 0);
    CHECK(strcmp(lichen_manifest_err_text(LICHEN_MANIFEST_ERR_DUPLICATE), "duplicate key") == 0);
    CHECK(strcmp(lichen_manifest_err_text((lichen_manifest_err_t)99), "?") == 0);
}

int main(void) {
    test_good();
    test_defaults_and_endings();
    test_refused();
    test_long_and_nul();
    test_compatible();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("lichen_manifest_test: all passed\n");
    return 0;
}
