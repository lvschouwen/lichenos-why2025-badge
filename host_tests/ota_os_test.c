// Host tests for ota_os.c (#245): the update server reaches the settings and the OS's Wi-Fi switch only through what
// lichen_os binds, and every unbound call has a defined default
#include "ota_os.h"

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

static char    last_key[32];
static uint8_t last_u8;
static char    last_str[32];
static bool    wifi;

static uint8_t get_u8(char const *key, uint8_t def) {
    snprintf(last_key, sizeof(last_key), "%s", key);
    return (uint8_t)(def + 7);
}
static bool set_u8(char const *key, uint8_t value) {
    snprintf(last_key, sizeof(last_key), "%s", key);
    last_u8 = value;
    return true;
}
static bool secret_get(char const *key, char *out, size_t cap) {
    snprintf(last_key, sizeof(last_key), "%s", key);
    snprintf(out, cap, "stored");
    return true;
}
static bool secret_set(char const *key, char const *value) {
    snprintf(last_key, sizeof(last_key), "%s", key);
    snprintf(last_str, sizeof(last_str), "%s", value);
    return true;
}
static bool wifi_on(void) {
    return wifi;
}

static void unbound_defaults(void) {
    char out[8];
    ota_os_bind(NULL);
    CHECK(ota_os_get_u8("ota_wifi", 3) == 3);
    CHECK(!ota_os_set_u8("ota_wifi", 1));
    strcpy(out, "junk");
    CHECK(!ota_os_secret_get("ota_token", out, sizeof(out))); // nothing bound: not found, out emptied
    CHECK(strcmp(out, "") == 0);
    out[0] = 'x';
    CHECK(!ota_os_secret_get("ota_token", out, 0)); // no room: untouched
    CHECK(out[0] == 'x');
    CHECK(!ota_os_secret_set("ota_token", "t"));
    CHECK(!ota_os_wifi_on()); // no switch bound: the server does not serve
}

static void bound_calls_pass_through(void) {
    ota_os_t const os = {
        .get_u8 = get_u8, .set_u8 = set_u8, .secret_get = secret_get, .secret_set = secret_set, .wifi_on = wifi_on};
    char out[16];
    ota_os_bind(&os);
    CHECK(ota_os_get_u8("ota_auto", 1) == 8 && strcmp(last_key, "ota_auto") == 0);
    CHECK(ota_os_set_u8("ota_wifi", 0) && last_u8 == 0 && strcmp(last_key, "ota_wifi") == 0);
    CHECK(ota_os_secret_get("ota_token", out, sizeof(out)) && strcmp(out, "stored") == 0);
    CHECK(ota_os_secret_set("ota_token", "abcd") && strcmp(last_str, "abcd") == 0);
    wifi = true;
    CHECK(ota_os_wifi_on());
    wifi = false;
    CHECK(!ota_os_wifi_on());
}

static void null_member_defaults(void) {
    ota_os_t const os = {.wifi_on = wifi_on};
    ota_os_bind(&os);
    wifi = true;
    CHECK(ota_os_wifi_on());
    CHECK(ota_os_get_u8("ota_wifi", 5) == 5);
    CHECK(!ota_os_set_u8("ota_wifi", 1));
}

int main(void) {
    unbound_defaults();
    bound_calls_pass_through();
    null_member_defaults();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ota_os_test: all passed\n");
    return 0;
}
