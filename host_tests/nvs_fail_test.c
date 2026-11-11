// Host test for fw_console/nvs_fail.c (#325): a failed NVS write of an early-boot owner is reported to the bound sink,
// one report per failure; what happens before the bind is kept and replayed; a missing key on erase is not a failure
#include "nvs_fail.h"

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

static int  calls;
static int  last_err;
static char last_key[16];

static void sink(char const *key, int err) {
    calls++;
    last_err = err;
    snprintf(last_key, sizeof(last_key), "%s", key);
}

static void ok_is_silent(void) {
    fw_nvs_fail_reset();
    fw_nvs_fail_bind(sink);
    calls = 0;
    CHECK(fw_nvs_ok("t", "k", 0));
    CHECK(fw_nvs_erased("t", "k", 0x1102)); // ESP_ERR_NVS_NOT_FOUND: already gone
    CHECK(calls == 0);
}

static void failure_is_reported_once(void) {
    fw_nvs_fail_reset();
    fw_nvs_fail_bind(sink);
    calls = 0;
    CHECK(!fw_nvs_ok("t", "crash_count", 0x1105));
    CHECK(calls == 1 && last_err == 0x1105 && strcmp(last_key, "crash_count") == 0);
    CHECK(!fw_nvs_erased("t", "c6_force", 0x1106));
    CHECK(calls == 2 && last_err == 0x1106);
}

static void early_failures_replay_on_bind(void) {
    fw_nvs_fail_reset();
    calls = 0;
    CHECK(!fw_nvs_ok("t", "dump_sum", -1));
    CHECK(!fw_nvs_ok("t", "crash_count", 0x1105));
    CHECK(calls == 0); // nobody to tell yet
    fw_nvs_fail_bind(sink);
    CHECK(calls == 2 && last_err == 0x1105 && strcmp(last_key, "crash_count") == 0);
    fw_nvs_fail_bind(sink); // a second bind replays nothing
    CHECK(calls == 2);
    for (int i = 0; i < FW_NVS_FAIL_PENDING + 3; i++) { // unbound again: the kept list is bounded
        fw_nvs_fail_bind(NULL);
        fw_nvs_ok("t", "k", 1);
    }
    calls = 0;
    fw_nvs_fail_reset();
    for (int i = 0; i < FW_NVS_FAIL_PENDING + 3; i++) {
        fw_nvs_ok("t", "k", 1);
    }
    fw_nvs_fail_bind(sink);
    CHECK(calls == FW_NVS_FAIL_PENDING);
}

int main(void) {
    ok_is_silent();
    failure_is_reported_once();
    early_failures_replay_on_bind();
    if (failures) {
        return 1;
    }
    printf("nvs_fail_test: ok\n");
    return 0;
}
