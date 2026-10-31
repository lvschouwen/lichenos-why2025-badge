// Host test: Wi-Fi credential rules and the NVS round-trip (#66, Review Focus 1)
#include "nvs.h"
#include "wifi_list.h"

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

static char const *text(char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (char)('a' + i % 26);
    }
    buf[len] = '\0';
    return buf;
}

int main(void) {
    char s[40], p[80];
    CHECK(!wifi_cred_valid(text(s, 0), "password"));
    CHECK(wifi_cred_valid(text(s, 1), "password"));
    CHECK(wifi_cred_valid(text(s, 32), "password"));
    CHECK(!wifi_cred_valid(text(s, 33), "password"));
    CHECK(wifi_cred_valid("net", text(p, 0))); // open network
    CHECK(!wifi_cred_valid("net", text(p, 7)));
    CHECK(wifi_cred_valid("net", text(p, 8)));
    CHECK(wifi_cred_valid("net", text(p, 63)));
    CHECK(!wifi_cred_valid("net", text(p, 64)));
    CHECK(!wifi_cred_valid(NULL, "password") && !wifi_cred_valid("net", NULL));

    // 63 bytes with a space, a double quote and a backslash survive the save and load byte for byte
    char pass[64];
    memset(pass, 'x', 63);
    pass[63] = '\0';
    pass[5]  = ' ';
    pass[20] = '"';
    pass[40] = '\\';
    pass[62] = ' ';
    uint32_t gen = wifi_cred_generation();
    CHECK(wifi_cred_save("my net", pass));
    CHECK(wifi_cred_generation() == gen + 1);
    char ssid_back[33], pass_back[64];
    CHECK(wifi_cred_load(ssid_back, pass_back));
    CHECK(strcmp(ssid_back, "my net") == 0);
    CHECK(memcmp(pass_back, pass, 64) == 0);

    // Open network: empty password is stored and read back as empty
    CHECK(wifi_cred_save("cafe", ""));
    CHECK(wifi_cred_load(ssid_back, pass_back) && strcmp(ssid_back, "cafe") == 0 && pass_back[0] == '\0');

    // Invalid input is refused and leaves the saved pair alone
    CHECK(!wifi_cred_save("cafe", "short"));
    CHECK(wifi_cred_load(ssid_back, pass_back) && strcmp(ssid_back, "cafe") == 0);

    // A write that fails half way keeps the old pair (no new SSID with the old password) and does not count as a change
    CHECK(wifi_cred_save("old", "oldpassword"));
    gen = wifi_cred_generation();
    nvs_stub_fail_set(2); // the password write
    CHECK(!wifi_cred_save("new", "newpassword"));
    CHECK(wifi_cred_load(ssid_back, pass_back) && strcmp(ssid_back, "old") == 0 && strcmp(pass_back, "oldpassword") == 0);
    CHECK(wifi_cred_generation() == gen);
    // With nothing saved before, a failed save leaves nothing behind
    CHECK(wifi_cred_clear());
    nvs_stub_fail_set(2);
    CHECK(!wifi_cred_save("new", "newpassword"));
    CHECK(!wifi_cred_load(ssid_back, pass_back));

    CHECK(wifi_cred_clear());
    CHECK(!wifi_cred_load(ssid_back, pass_back));

    if (failures) {
        return 1;
    }
    printf("wifi_cred_test: ok\n");
    return 0;
}
