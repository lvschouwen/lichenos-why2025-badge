// Host test: the settings service's write-failure counter and last error (#320)
#include "settings_fail.h"
#include "settings_reg.h"

#include <stdint.h>
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

static void test_counts_and_latest(void) {
    char t[16];
    settings_fail_reset();
    CHECK(settings_fail_count() == 0);
    settings_fail_last_text(t, sizeof(t));
    CHECK(t[0] == '\0');
    settings_fail_record("bl_disp", 0x1106, 40);
    settings_fail_record("tz", 0x101, 77);
    CHECK(settings_fail_count() == 2);
    settings_fail_last_text(t, sizeof(t));
    CHECK(strcmp(t, "tz 0x101") == 0);
    CHECK(settings_fail_last_err() == 0x101);
    CHECK(settings_fail_last_uptime_s() == 77);
}

// A 15-character NVS key plus the code does not fit REG_STR_MAX: the key is cut, the code stays whole
static void test_truncates_key(void) {
    char t[16];
    settings_fail_reset();
    settings_fail_record("abcdefghijklmno", 0x1106, 1);
    settings_fail_last_text(t, sizeof(t));
    CHECK(strlen(t) <= 15);
    CHECK(strcmp(t, "abcdefgh 0x1106") == 0);
    settings_fail_record("k", -1, 2); // ESP_FAIL
    settings_fail_last_text(t, sizeof(t));
    CHECK(strlen(t) <= 15);
    CHECK(strcmp(t, "k 0xffffffff") == 0);
}

static void test_saturates(void) {
    settings_fail_reset();
    settings_fail_preset_count(UINT32_MAX - 1);
    settings_fail_record("k", 1, 0);
    CHECK(settings_fail_count() == UINT32_MAX);
    settings_fail_record("k", 2, 0);
    CHECK(settings_fail_count() == UINT32_MAX); // stays at the top, no wrap to 0
    CHECK(settings_fail_last_err() == 2);       // the latest is still kept
}

static void test_registry_getters(void) {
    reg_value_t v;
    settings_fail_reset();
    settings_fail_record("wifi_ssid_0", 0x1106, 5);
    CHECK(settings_reg_write_fail(NULL, &v) == REG_OK && v.u == 1);
    CHECK(settings_reg_last_err(NULL, &v) == REG_OK && strcmp(v.s, "wifi_ssi 0x1106") == 0);
}

int main(void) {
    test_counts_and_latest();
    test_truncates_key();
    test_saturates();
    test_registry_getters();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("settings_fail_test: ok\n");
    return 0;
}
