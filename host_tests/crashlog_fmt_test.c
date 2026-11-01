// Host test: the crash log viewer's parsing, rows, full view, delete-one rewrite and the "new since last seen" rule (#72)
#include "crash_log.h"
#include "crashlog_fmt.h"

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

#define DOT "\xE2\x80\xA2"

static void test_parse(void) {
    char           line[256];
    uint32_t const bt[3] = {0x40001234, 0x4ff00010, 0xdeadbeef};
    // The writer's own output (crash_log.c) is what the reader must understand
    crash_record_format(line, sizeof(line), "2026-10-30T12:00:00", "fw-v0.1.0-3-gabc1234", "1a2b3c4d5", "panic", "console",
                        0x4ff0abcd, bt, 3);
    crashlog_entry_t e;
    crashlog_parse(line, strlen(line), &e);
    CHECK(e.parsed);
    CHECK(strcmp(e.when, "2026-10-30T12:00:00") == 0);
    CHECK(strcmp(e.version, "fw-v0.1.0-3-gabc1234") == 0);
    CHECK(strcmp(e.reason, "panic") == 0);
    CHECK(strcmp(e.task, "console") == 0);
    CHECK(e.pc == 0x4ff0abcd);
    CHECK(strcmp(e.elf, "1a2b3c4d5") == 0);
    CHECK(e.depth == 3 && e.bt[0] == 0x40001234 && e.bt[2] == 0xdeadbeef);
    CHECK(strcmp(e.line, line) == 0);

    // Without bt and elf, clock not set
    crash_record_format(line, sizeof(line), "#3", "v", NULL, "task_wdt", "mesh", 0x10, NULL, 0);
    crashlog_parse(line, strlen(line), &e);
    CHECK(e.parsed && e.depth == 0 && e.elf[0] == '\0' && e.pc == 0x10 && strcmp(e.when, "#3") == 0);

    // Only n bytes count (a line inside the log text)
    crashlog_parse("t v wdt task=x pc=0x1\nnext", 21, &e);
    CHECK(e.parsed && strcmp(e.line, "t v wdt task=x pc=0x1") == 0 && e.pc == 1);

    // Not a record: kept as text
    crashlog_parse("garbage", 7, &e);
    CHECK(!e.parsed && strcmp(e.line, "garbage") == 0 && e.reason[0] == '\0');

    // A bt list longer than the array is cut, never overrun
    char long_bt[256] = "t v panic task=x pc=0x1 bt=";
    for (int i = 0; i < 20; i++) {
        strcat(long_bt, i ? ",0x1" : "0x1");
    }
    crashlog_parse(long_bt, strlen(long_bt), &e);
    CHECK(e.depth == CRASHLOG_BT_MAX);
}

static void test_text(void) {
    char out[1024], val[32];
    crashlog_when_text("2026-10-30T12:34:56", out, sizeof(out));
    CHECK(strcmp(out, "2026-10-30 12:34 UTC") == 0);
    crashlog_when_text("#3", out, sizeof(out));
    CHECK(strcmp(out, "#3 (clock not set)") == 0);
    CHECK(strcmp(crashlog_reason_text("int_wdt"), "Interrupt watchdog") == 0);
    CHECK(strcmp(crashlog_reason_text("weird"), "weird") == 0);

    crashlog_entry_t e;
    char const      *l = "2026-10-30T12:34:56 fw-1 panic task=console pc=0x4ff0abcd elf=abc bt=0x1,0x2";
    crashlog_parse(l, strlen(l), &e);
    crashlog_row(&e, out, sizeof(out), val, sizeof(val));
    CHECK(strcmp(out, "Panic " DOT " 2026-10-30 12:34 UTC") == 0);
    CHECK(strcmp(val, "console") == 0);

    size_t n = crashlog_full(&e, true, out, sizeof(out));
    CHECK(n == strlen(out));
    CHECK(strstr(out, "When: 2026-10-30 12:34 UTC\n") != NULL);
    CHECK(strstr(out, "Reset reason: Panic (panic)\n") != NULL);
    CHECK(strstr(out, "Task: console\nPC: 0x4ff0abcd\nFirmware: fw-1\n") != NULL);
    CHECK(strstr(out, "Crashed image ELF: abc\n") != NULL);
    CHECK(strstr(out, "Backtrace:\n  0x00000001\n  0x00000002\n") != NULL);
    CHECK(strstr(out, "Core dump:") != NULL);
    CHECK(strstr(out, l) != NULL);
    crashlog_full(&e, false, out, sizeof(out));
    CHECK(strstr(out, "Core dump:") == NULL);

    // A short buffer stays terminated
    n = crashlog_full(&e, true, out, 20);
    CHECK(n == 19 && strlen(out) == 19);

    crashlog_parse("junk", 4, &e);
    crashlog_row(&e, out, sizeof(out), val, sizeof(val));
    CHECK(strcmp(out, "junk") == 0 && val[0] == '\0');
}

static void test_split_remove(void) {
    char            text[256] = "a1 v panic\n\nb2 v wdt\r\nc3 v panic"; // blank line, CRLF, no final newline
    size_t          len       = strlen(text);
    crashlog_span_t s[CRASHLOG_MAX];
    size_t          n = crashlog_split(text, len, s, CRASHLOG_MAX);
    CHECK(n == 3);
    CHECK(strncmp(text + s[0].start, "c3 v panic", s[0].len) == 0 && s[0].len == 10); // newest first
    CHECK(strncmp(text + s[1].start, "b2 v wdt", s[1].len) == 0 && s[1].len == 8);
    CHECK(strncmp(text + s[2].start, "a1 v panic", s[2].len) == 0);

    // max keeps the newest
    n = crashlog_split(text, len, s, 2);
    CHECK(n == 2 && strncmp(text + s[0].start, "c3", 2) == 0 && strncmp(text + s[1].start, "b2", 2) == 0);
    CHECK(crashlog_split("", 0, s, CRASHLOG_MAX) == 0);
    CHECK(crashlog_split("\n\n", 2, s, CRASHLOG_MAX) == 0);

    // Remove the middle one (index 1 = b2) with its CRLF
    CHECK(crashlog_remove(text, &len, 1));
    CHECK(strcmp(text, "a1 v panic\n\nc3 v panic") == 0 && len == strlen(text));
    // The newest (no final newline)
    CHECK(crashlog_remove(text, &len, 0));
    CHECK(strcmp(text, "a1 v panic\n\n") == 0 && len == strlen(text));
    CHECK(!crashlog_remove(text, &len, 1));
    CHECK(crashlog_remove(text, &len, 0));
    CHECK(len == 1 && strcmp(text, "\n") == 0);
    CHECK(!crashlog_remove(text, &len, 0));

    // A real log: delete the oldest of three
    strcpy(text, "x1 v panic task=a pc=0x1\nx2 v panic task=b pc=0x2\nx3 v panic task=c pc=0x3\n");
    len = strlen(text);
    CHECK(crashlog_remove(text, &len, 2));
    CHECK(strcmp(text, "x2 v panic task=b pc=0x2\nx3 v panic task=c pc=0x3\n") == 0);
}

static void test_unseen(void) {
    uint32_t const h = 0x1234, other = 0x9999;
    CHECK(crashlog_unseen(0, 0, 0, 0, 0) == 0);
    CHECK(crashlog_unseen(1, 0, h, 0, 1) == 1);        // first crash ever
    CHECK(crashlog_unseen(5, 5, h, h, 5) == 0);        // nothing since the last open
    CHECK(crashlog_unseen(7, 5, h, other, 7) == 2);    // two more
    CHECK(crashlog_unseen(30, 5, h, other, 20) == 20); // never more than stored
    CHECK(crashlog_unseen(1, 5, h, other, 1) == 1);    // fw crash clear, then one crash
    CHECK(crashlog_unseen(5, 5, h, other, 5) == 5);    // fw crash clear, then as many crashes as before
    CHECK(crashlog_unseen(5, 5, h, h, 0) == 0);        // log gone
    CHECK(crashlog_unseen(3, 0, h, 0, 0) == 0);

    CHECK(crashlog_hash("abc", 3) != crashlog_hash("abd", 3));
    CHECK(crashlog_hash("", 0) == 2166136261u);

    char b[8];
    crashlog_badge(0, b, sizeof(b));
    CHECK(b[0] == '\0');
    crashlog_badge(7, b, sizeof(b));
    CHECK(strcmp(b, "7") == 0);
    crashlog_badge(99, b, sizeof(b));
    CHECK(strcmp(b, "99") == 0);
    crashlog_badge(100, b, sizeof(b));
    CHECK(strcmp(b, "99+") == 0);
}

static void test_tile(void) {
    char             l1[64], l2[64];
    crashlog_entry_t e;
    char const      *line = "#2 v brownout task=x pc=0x0";
    crashlog_parse(line, strlen(line), &e);
    crashlog_tile(0, 0, NULL, l1, sizeof(l1), l2, sizeof(l2));
    CHECK(strcmp(l1, "No crashes stored") == 0 && l2[0] == '\0');
    crashlog_tile(1, 0, &e, l1, sizeof(l1), l2, sizeof(l2));
    CHECK(strcmp(l1, "1 crash") == 0);
    CHECK(strcmp(l2, "Brownout " DOT " #2 (clock not set)") == 0);
    crashlog_tile(5, 2, &e, l1, sizeof(l1), l2, sizeof(l2));
    CHECK(strcmp(l1, "2 new " DOT " 5 crashes") == 0);
    crashlog_tile(5, 120, &e, l1, sizeof(l1), l2, sizeof(l2));
    CHECK(strcmp(l1, "99+ new " DOT " 5 crashes") == 0);
}

int main(void) {
    test_parse();
    test_text();
    test_split_remove();
    test_unseen();
    test_tile();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("crashlog_fmt_test: all passed\n");
    return 0;
}
