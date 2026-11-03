// Host test: Wi-Fi credential rules and the NVS round-trip (#66, Review Focus 1)
#include "nvs.h"
#include "wifi_list.h"
#include "wifi_saved.h"

#include <stdbool.h>
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


// #160: the pre-existing single pair becomes entry 1 on the first load, and is not lost when another is joined
static void saved_networks(void) {
    char s[33], p[64];
    wifi_cred_clear();
    nvs_handle_t nvs;
    nvs_open("badge_fw", NVS_READWRITE, &nvs); // what an older firmware left: only wifi_ssid / wifi_pass
    nvs_set_str(nvs, "wifi_ssid", "homenet");
    nvs_set_str(nvs, "wifi_pass", "homepassword");
    wifi_saved_t l;
    CHECK(wifi_saved_load(&l) && l.n == 1 && strcmp(l.e[0].ssid, "homenet") == 0 && strcmp(l.e[0].pass, "homepassword") == 0);
    nvs_erase_key(nvs, "wifi_s0"); // migration wrote it: prove that by erasing it and migrating again
    CHECK(wifi_saved_load(&l) && l.n == 1);
    CHECK(wifi_saved_load(&l) && l.n == 1); // loaded from the slot now, not migrated twice

    // Joining a second network keeps the first; the new one is the pair in use and first in the list
    CHECK(wifi_cred_save("campnet", "camppassword"));
    CHECK(wifi_saved_load(&l) && l.n == 2 && strcmp(l.e[0].ssid, "campnet") == 0 && strcmp(l.e[1].ssid, "homenet") == 0);
    CHECK(wifi_cred_load(s, p) && strcmp(s, "campnet") == 0);

    // Migration with the old pair never saved a list: save() migrates it before overwriting the active pair
    wifi_cred_clear();
    nvs_set_str(nvs, "wifi_ssid", "oldnet");
    nvs_set_str(nvs, "wifi_pass", "oldpassword");
    CHECK(wifi_cred_save("newnet", "newpassword"));
    CHECK(wifi_saved_load(&l) && l.n == 2 && strcmp(l.e[1].ssid, "oldnet") == 0 && strcmp(l.e[1].pass, "oldpassword") == 0);

    // Forgetting the pair in use makes the next one the pair in use; the last one leaves nothing
    bool active = false, left = false;
    CHECK(wifi_cred_forget("newnet", &active, &left) && active && left);
    CHECK(wifi_cred_load(s, p) && strcmp(s, "oldnet") == 0 && strcmp(p, "oldpassword") == 0);
    CHECK(!wifi_cred_forget("newnet", &active, &left)); // already gone
    CHECK(wifi_cred_forget("oldnet", &active, &left) && active && !left);
    CHECK(!wifi_cred_load(s, p));
    CHECK(wifi_saved_load(&l) && l.n == 0);

    // Forgetting one that is not in use leaves the pair in use alone; activate moves a saved one to the front
    CHECK(wifi_cred_save("a-net", "passwordaaa"));
    CHECK(wifi_cred_save("b-net", "passwordbbb"));
    CHECK(wifi_cred_save("c-net", "passwordccc"));
    CHECK(wifi_cred_forget("a-net", &active, &left) && !active && left);
    CHECK(wifi_cred_load(s, p) && strcmp(s, "c-net") == 0);
    CHECK(wifi_cred_activate("b-net") && wifi_cred_load(s, p) && strcmp(s, "b-net") == 0 && strcmp(p, "passwordbbb") == 0);
    CHECK(wifi_saved_load(&l) && l.n == 2 && strcmp(l.e[0].ssid, "b-net") == 0);
    CHECK(!wifi_cred_activate("nope"));

    // The factory reset and "fw wifi --clear" remove every saved network, not only the one in use
    CHECK(wifi_cred_clear());
    CHECK(wifi_saved_load(&l) && l.n == 0 && !wifi_cred_load(s, p));
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

    saved_networks();

    if (failures) {
        return 1;
    }
    printf("wifi_cred_test: ok\n");
    return 0;
}
