// Host tests for ota_hw.c (#321): the hw object of /info is always valid JSON, nulls for what the cache cannot say
#include "ota_hw.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static size_t fake(char const *name, char *buf, size_t cap) {
    char const *t = NULL;
    if (!strcmp(name, "psram.clk")) {
        t = "120 (3.2 s ago)";
    } else if (!strcmp(name, "disp.pclk")) {
        t = "? (not read yet)";
    } else if (!strcmp(name, "tz.zone")) {
        t = "Europe/\"Ams\"\\\n (stale, 1.0 s ago)";
    } else if (!strcmp(name, "bt.state")) {
        t = "idle";
    }
    if (!t) {
        return 0; // a key the table does not have
    }
    snprintf(buf, cap, "%s", t);
    return strlen(buf);
}

static void test_value(void) {
    char v[32];
    CHECK(ota_hw_value("120 (3.2 s ago)", v, sizeof(v)) && !strcmp(v, "120"));
    CHECK(ota_hw_value("a (b) (stale, 1.0 s ago)", v, sizeof(v)) && !strcmp(v, "a (b)"));
    CHECK(ota_hw_value("idle", v, sizeof(v)) && !strcmp(v, "idle"));
    CHECK(!ota_hw_value("? (no answer)", v, sizeof(v)));
    CHECK(!ota_hw_value("", v, sizeof(v)));
    CHECK(!ota_hw_value("0123456789 (1 s ago)", v, 5)); // does not fit
}

static void test_json(void) {
    char   b[1024];
    size_t n = ota_hw_json(fake, b, sizeof(b));
    CHECK(n == strlen(b));
    CHECK(strstr(b, "\"psram.clk\":\"120\""));
    CHECK(strstr(b, "\"disp.pclk\":null"));
    CHECK(strstr(b, "\"sens.acc_odr\":null"));
    CHECK(strstr(b, "\"bt.state\":\"idle\""));
    CHECK(strstr(b, "\"tz.zone\":\"Europe/\\\"Ams\\\"\\\\\\u000a\""));
    CHECK(b[0] == '{' && b[n - 1] == '}');
    CHECK(strstr(b, "\"settings.write_fail\":null"));
    CHECK(ota_hw_json(NULL, b, sizeof(b)) > 0 && strstr(b, "\"psram.clk\":null"));
}

// Whatever the buffer size: NUL terminated, braces closed, no half member
static void test_truncation(void) {
    char full[1024];
    ota_hw_json(fake, full, sizeof(full));
    for (size_t cap = 3; cap < sizeof(full); cap++) {
        char   b[1024];
        size_t n = ota_hw_json(fake, b, cap);
        CHECK(n < cap && b[n] == 0 && b[0] == '{' && b[n - 1] == '}');
        int q = 0; // quotes balanced
        for (size_t i = 0; i < n; i++) {
            q += b[i] == '"' && (i == 0 || b[i - 1] != '\\');
        }
        CHECK(q % 2 == 0);
    }
    char b[2] = "x";
    CHECK(ota_hw_json(fake, b, 2) == 0 && b[0] == 0);
    CHECK(ota_hw_json(fake, b, 0) == 0);
}

static void test_table(void) {
    for (size_t i = 0; i < ota_hw_key_count(); i++) {
        char const *k = ota_hw_key(i);
        CHECK(!strstr(k, "token") && (!strstr(k, "key") || strstr(k, "keyboard")) && !strstr(k, "pass") && !strstr(k, "secret"));
    }
    CHECK(!ota_hw_key(ota_hw_key_count()));
}

int main(void) {
    test_value();
    test_json();
    test_truncation();
    test_table();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ota_hw_test ok\n");
    return 0;
}
