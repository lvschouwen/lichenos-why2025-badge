// Host test: crash record text, log trimming, base64 lines (#74)
#include "crash_log.h"

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

int main(void) {
    char           out[256];
    uint32_t const bt[3] = {0x40001234, 0x4ff00010, 0xdeadbeef};

    size_t n = crash_record_format(out, sizeof(out), "2026-10-30T12:00:00", "fw-v0.1.0-3-gabc1234", "1a2b3c4d5", "panic", "console", 0x4ff0abcd, bt, 3);
    CHECK(strcmp(out, "2026-10-30T12:00:00 fw-v0.1.0-3-gabc1234 panic task=console pc=0x4ff0abcd elf=1a2b3c4d5 bt=0x40001234,0x4ff00010,0xdeadbeef") == 0);
    CHECK(n == strlen(out));

    crash_record_format(out, sizeof(out), "t", "v", NULL, "wdt", "mesh", 0x10, NULL, 0);
    CHECK(strcmp(out, "t v wdt task=mesh pc=0x00000010") == 0);

    // The task name keeps [A-Za-z0-9_-], 16 chars at most
    crash_record_format(out, sizeof(out), "t", "v", NULL, "panic", "mesh task!", 0, NULL, 0);
    CHECK(strcmp(out, "t v panic task=mesh_task_ pc=0x00000000") == 0);
    crash_record_format(out, sizeof(out), "t", "v", NULL, "panic", "abcdefghijklmnopqrstuvwxyz", 0, NULL, 0);
    CHECK(strstr(out, "task=abcdefghijklmnop pc=") != NULL);

    // Never overruns a short buffer
    n = crash_record_format(out, 10, "2026-10-30T12:00:00", "v", "", "panic", "x", 0, bt, 3);
    CHECK(strlen(out) == 9 && n == 9);

    // The crashed image's ELF SHA (from the core dump) is hex only: anything else is cut there; empty or NULL leaves it out
    crash_record_format(out, sizeof(out), "t", "v", "", "panic", "x", 0, NULL, 0);
    CHECK(strcmp(out, "t v panic task=x pc=0x00000000") == 0);
    crash_record_format(out, sizeof(out), "t", "v", "ABCdef012 x", "panic", "x", 0, NULL, 0);
    CHECK(strcmp(out, "t v panic task=x pc=0x00000000 elf=abcdef012") == 0);
    crash_record_format(out, sizeof(out), "t", "v", "zz", "panic", "x", 0, NULL, 0);
    CHECK(strcmp(out, "t v panic task=x pc=0x00000000") == 0);

    char sha[16];
    CHECK(crash_elf_sha_clean(sha, sizeof(sha), "1A2b3c4d5") == 9 && strcmp(sha, "1a2b3c4d5") == 0);
    CHECK(crash_elf_sha_clean(sha, sizeof(sha), "12\n34") == 2 && strcmp(sha, "12") == 0);
    CHECK(crash_elf_sha_clean(sha, 4, "123456") == 3 && strcmp(sha, "123") == 0);
    CHECK(crash_elf_sha_clean(sha, sizeof(sha), NULL) == 0 && sha[0] == '\0');

    // Trim keeps the last lines
    char   text[512] = "";
    size_t len       = 0;
    for (int i = 1; i <= 25; i++) {
        len += (size_t)snprintf(text + len, sizeof(text) - len, "line%d\n", i);
    }
    len = crash_log_trim(text, len, 20);
    CHECK(len == strlen(text));
    CHECK(strncmp(text, "line6\n", 6) == 0);
    CHECK(strcmp(text + len - 7, "line25\n") == 0);

    strcpy(text, "a\nb\nc\n");
    len = crash_log_trim(text, 6, 20);
    CHECK(len == 6 && strcmp(text, "a\nb\nc\n") == 0);

    // A last line without a newline still counts
    strcpy(text, "a\nb\nc");
    len = crash_log_trim(text, 5, 2);
    CHECK(len == 3 && strncmp(text, "b\nc", 3) == 0);

    // Base64
    char b64[400];
    CHECK(crash_b64_lines((uint8_t const *)"Man", 3, b64, sizeof(b64), 76) == 4);
    CHECK(strcmp(b64, "TWFu") == 0);
    CHECK(crash_b64_lines((uint8_t const *)"Ma", 2, b64, sizeof(b64), 76) == 4 && strcmp(b64, "TWE=") == 0);
    CHECK(crash_b64_lines((uint8_t const *)"M", 1, b64, sizeof(b64), 76) == 4 && strcmp(b64, "TQ==") == 0);

    uint8_t zeros[100] = {0};
    size_t  bn         = crash_b64_lines(zeros, 100, b64, sizeof(b64), 76);
    CHECK(bn == 136 + 1); // 136 base64 chars and one newline after char 76
    CHECK(b64[76] == '\n');
    CHECK(strchr(b64 + 77, '\n') == NULL);
    CHECK(b64[bn] == '\0');

    // Too small a buffer: 0
    CHECK(crash_b64_lines((uint8_t const *)"Man", 3, b64, 4, 76) == 0); // needs room for the NUL
    CHECK(crash_b64_lines(zeros, 100, b64, 100, 76) == 0);

    // Which resets are crashes
    CHECK(crash_is_crash(CRASH_RST_PANIC, false));
    CHECK(crash_is_crash(CRASH_RST_INT_WDT, false));
    CHECK(crash_is_crash(CRASH_RST_TASK_WDT, false));
    CHECK(crash_is_crash(CRASH_RST_BROWNOUT, false));
    CHECK(!crash_is_crash(CRASH_RST_WDT, false)); // a USB reset after a flash
    CHECK(crash_is_crash(CRASH_RST_WDT, true));
    CHECK(!crash_is_crash(CRASH_RST_OTHER, false));
    CHECK(!crash_is_crash(CRASH_RST_OTHER, true)); // power-on or restart with an old dump on flash

    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("crash_log_test: all passed\n");
    return 0;
}
