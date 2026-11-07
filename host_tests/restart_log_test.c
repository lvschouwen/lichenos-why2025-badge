// Host test: the restart log ring and line format (#41), the restart save plan (#123)
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

static unsigned char blob[sizeof(restart_log_t)];
static size_t        blob_len;
static char          last_key[16];
static bool          fake_get(char const *key, void *out, size_t size) {
    if (strcmp(key, "rst_log") != 0 || size != blob_len) {
        return false;
    }
    memcpy(out, blob, size);
    return true;
}
static bool fake_set(char const *key, void const *data, size_t size) {
    snprintf(last_key, sizeof(last_key), "%s", key);
    memcpy(blob, data, size);
    blob_len = size;
    return true;
}

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

    // #123: the first restart runs the saves, a second one waits for it, none without a hook
    CHECK(restart_save_plan(true, false) == RESTART_SAVE_RUN);
    CHECK(restart_save_plan(false, false) == RESTART_SAVE_NONE);
    CHECK(restart_save_plan(true, true) == RESTART_SAVE_WAIT);
    CHECK(restart_save_plan(false, true) == RESTART_SAVE_WAIT);

    // #235: the history is kept by the settings service as one blob under the badge_fw key from before the move
    CHECK(strcmp(RESTART_LOG_KEY, "rst_log") == 0);
    CHECK(restart_log_load(NULL, &log) == 0 && log.count == 0); // not bound yet
    memset(blob, 0, sizeof(blob));
    CHECK(!restart_log_store(NULL, &log));
    restart_log_push(&log, "kept");
    CHECK(restart_log_store(fake_set, &log) && blob_len == sizeof(log) && strcmp(last_key, "rst_log") == 0);
    restart_log_t back;
    CHECK(restart_log_load(fake_get, &back) == 1 && strcmp(back.line[0], "kept") == 0);
    blob_len--; // a blob of another size is not ours: empty, not a half-read log
    CHECK(restart_log_load(fake_get, &back) == 0 && back.count == 0);
    blob_len = sizeof(log) + 1; // a larger blob is not ours either
    CHECK(restart_log_load(fake_get, &back) == 0 && back.count == 0);
    blob_len = 0;
    CHECK(restart_log_load(fake_get, &back) == 0); // nothing saved

    if (failures == 0) {
        printf("restart_log_test: ok\n");
    }
    return failures ? 1 : 0;
}
