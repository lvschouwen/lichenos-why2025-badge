// Host test: the restart log ring and line format (#41)
#include "restart_log.h"

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
    char out[RESTART_LINE_MAX];
    restart_line_format(out, sizeof(out), "2026-10-31T13:14:05Z", "fw-abc", 42, "console reboot", 0);
    CHECK(strcmp(out, "2026-10-31T13:14:05Z fw-abc up=42s console reboot") == 0);
    restart_line_format(out, sizeof(out), NULL, "fw-abc", 7, "unknown caller", 0x40001234);
    CHECK(strcmp(out, "t? fw-abc up=7s unknown caller caller=0x40001234") == 0);
    restart_line_format(out, sizeof(out), NULL, NULL, 0, "a\nb", 0);
    CHECK(strcmp(out, "t? ? up=0s a_b") == 0);
    char   small[24];
    size_t n = restart_line_format(small, sizeof(small), "t", "v", 1, "0123456789012345678901234567890123456789", 0);
    CHECK(n == strlen(small) && n < sizeof(small));

    restart_log_t log;
    memset(&log, 0, sizeof(log));
    char l[16];
    for (int i = 1; i <= 7; i++) {
        snprintf(l, sizeof(l), "r%d", i);
        restart_log_push(&log, l);
    }
    CHECK(log.count == RESTART_LOG_KEEP);
    CHECK(strcmp(log.line[0], "r7") == 0);
    CHECK(strcmp(log.line[4], "r3") == 0);

    memset(&log, 0xff, sizeof(log));
    restart_log_sanitize(&log);
    CHECK(log.count == RESTART_LOG_KEEP);
    CHECK(strlen(log.line[2]) < RESTART_LINE_MAX);

    if (failures == 0) {
        printf("restart_log_test: ok\n");
    }
    return failures ? 1 : 0;
}
