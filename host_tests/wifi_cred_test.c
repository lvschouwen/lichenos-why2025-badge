// Host test: Wi-Fi credential rules and the round trip through the settings service, passwords as secrets (#66, #235)
#include "nvs.h"
#include "settings_secret.h"
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


// The settings service as settings_bind_net() binds it: settings_secret.c (the real one, over the NVS stub) for the
// passwords, and the plain calls as settings_store.c makes them, refusing a secret key the same way
static void plain_get_str(char const *key, char *out, size_t cap, char const *def) {
    nvs_handle_t nvs;
    size_t       len = cap;
    if (!settings_key_secret("badge_fw", key) && nvs_open("badge_fw", NVS_READONLY, &nvs) == ESP_OK &&
        nvs_get_str(nvs, key, out, &len) == ESP_OK) {
        return;
    }
    snprintf(out, cap, "%s", def);
}

static bool plain_set_str(char const *key, char const *value) {
    nvs_handle_t nvs;
    return !settings_key_secret("badge_fw", key) && nvs_open("badge_fw", NVS_READWRITE, &nvs) == ESP_OK &&
           nvs_set_str(nvs, key, value) == ESP_OK;
}

static int erase_fail_in; // the nth plain erase from now on fails (1 = the next one); 0 switches it off

static bool plain_erase(char const *key) {
    if (erase_fail_in > 0 && --erase_fail_in == 0) {
        return false;
    }
    nvs_handle_t nvs;
    nvs_open("badge_fw", NVS_READWRITE, &nvs);
    esp_err_t err = nvs_erase_key(nvs, key);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

static uint8_t plain_get_u8(char const *key, uint8_t def) {
    nvs_handle_t nvs;
    uint8_t      v = def;
    nvs_open("badge_fw", NVS_READONLY, &nvs);
    return nvs_get_u8(nvs, key, &v) == ESP_OK ? v : def;
}

static bool plain_set_u8(char const *key, uint8_t value) {
    nvs_handle_t nvs;
    nvs_open("badge_fw", NVS_READWRITE, &nvs);
    return nvs_set_u8(nvs, key, value) == ESP_OK;
}

static wifi_store_t const test_store = {
    .get_str    = plain_get_str,
    .set_str    = plain_set_str,
    .erase      = plain_erase,
    .get_u8     = plain_get_u8,
    .set_u8     = plain_set_u8,
    .secret_get = settings_secret_get,
    .secret_set = settings_secret_set,
};

// #235: before the settings service binds its store nothing is read or written (and no driver wipe runs)
static void unbound(void) {
    char         s[33], p[64];
    wifi_saved_t l;
    CHECK(!wifi_cred_load(s, p) && s[0] == '\0' && p[0] == '\0');
    CHECK(!wifi_saved_load(&l) && l.n == 0);
    CHECK(!wifi_cred_save("net", "password"));
    CHECK(!wifi_cred_clear());
    CHECK(wifi_driver_wiped());
    CHECK(!wifi_driver_wiped_set(true)); // nothing to mark: the caller then skips the restore
}

// #235: the passwords are secrets (settings_secret.c), the SSIDs plain settings under their old names, so what an
// earlier firmware saved still reads; v2 migrates nothing, so a lone pair in use does not become a list entry
static void secrets_and_defaults(void) {
    char         s[33], p[64], v[64];
    wifi_saved_t l;
    CHECK(wifi_cred_clear());
    CHECK(wifi_saved_load(&l) && l.n == 0); // the empty list is the default
    CHECK(wifi_cred_save("homenet", "homepassword"));
    plain_get_str("wifi_pass", v, sizeof(v), "-");
    CHECK(strcmp(v, "-") == 0); // the plain path never hands a password out
    plain_get_str("wifi_p0", v, sizeof(v), "-");
    CHECK(strcmp(v, "-") == 0);
    CHECK(settings_secret_get("wifi_pass", v, sizeof(v)) && strcmp(v, "homepassword") == 0);
    CHECK(settings_secret_get("wifi_p0", v, sizeof(v)) && strcmp(v, "homepassword") == 0);
    plain_get_str("wifi_ssid", v, sizeof(v), "");
    CHECK(strcmp(v, "homenet") == 0);
    CHECK(!settings_secret_get("wifi_ssid", v, sizeof(v)) && v[0] == '\0'); // the secret API is for secrets only
    CHECK(!settings_secret_set("tz", "UTC0"));
    CHECK(!plain_set_str("wifi_pass", "leak"));

    // Saved by hand as an older firmware would leave it: the pair in use reads, the list stays empty
    CHECK(wifi_cred_clear());
    CHECK(plain_set_str("wifi_ssid", "oldnet") && settings_secret_set("wifi_pass", "oldpassword"));
    CHECK(wifi_cred_load(s, p) && strcmp(s, "oldnet") == 0 && strcmp(p, "oldpassword") == 0);
    CHECK(wifi_saved_load(&l) && l.n == 0);
    CHECK(wifi_saved_load(&l) && l.n == 0); // and loading never writes one in
    // An SSID without its password is no pair
    CHECK(plain_erase("wifi_pass"));
    CHECK(!wifi_cred_load(s, p));
}

// #160: the saved list, through the bound store
static void saved_networks(void) {
    char s[33], p[64];
    wifi_saved_t l;
    CHECK(wifi_cred_clear());
    CHECK(wifi_cred_save("homenet", "homepassword"));
    CHECK(wifi_saved_load(&l) && l.n == 1 && strcmp(l.e[0].ssid, "homenet") == 0 && strcmp(l.e[0].pass, "homepassword") == 0);

    // Joining a second network keeps the first; the new one is the pair in use and first in the list
    CHECK(wifi_cred_save("campnet", "camppassword"));
    CHECK(wifi_saved_load(&l) && l.n == 2 && strcmp(l.e[0].ssid, "campnet") == 0 && strcmp(l.e[1].ssid, "homenet") == 0);
    CHECK(wifi_cred_load(s, p) && strcmp(s, "campnet") == 0);

    // Forgetting the pair in use makes the next one the pair in use; the last one leaves nothing
    bool active = false, left = false;
    CHECK(wifi_cred_forget("campnet", &active, &left) && active && left);
    CHECK(wifi_cred_load(s, p) && strcmp(s, "homenet") == 0 && strcmp(p, "homepassword") == 0);
    CHECK(!wifi_cred_forget("campnet", &active, &left)); // already gone
    CHECK(wifi_cred_forget("homenet", &active, &left) && active && !left);
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

    // Forgetting the pair in use when the pair cannot be replaced: failure, but the caller still learns it was in use
    CHECK(wifi_cred_clear());
    CHECK(wifi_cred_save("x-net", "passwordxxx"));
    CHECK(wifi_cred_save("y-net", "passwordyyy"));
    nvs_stub_fail_set(3); // two writes for the list, then wifi_ssid
    CHECK(!wifi_cred_forget("y-net", &active, &left) && active);
    nvs_stub_fail_set(0);

    // A clear whose erase fails on one slot still erases every slot after it (wifi_ssid, wifi_pass, then wifi_s0 fails)
    CHECK(wifi_cred_save("x-net", "passwordxxx"));
    CHECK(wifi_cred_save("z-net", "passwordzzz"));
    erase_fail_in = 3;
    CHECK(!wifi_cred_clear());
    erase_fail_in = 0;
    char v[64];
    CHECK(!settings_secret_get("wifi_p0", v, sizeof(v)) && !settings_secret_get("wifi_p1", v, sizeof(v)));

    // The factory reset and "fw wifi --clear" remove every saved network, not only the one in use
    CHECK(wifi_cred_clear());
    CHECK(wifi_saved_load(&l) && l.n == 0 && !wifi_cred_load(s, p));
}

int main(void) {
    unbound();
    wifi_list_bind_store(&test_store);
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

    // The once-only wipe of the C6 driver's own copy (#223 moved it from c6link): not done until marked, and a forget
    // (wifi_cred_clear) does not make it run again
    CHECK(!wifi_driver_wiped());
    CHECK(wifi_driver_wiped_set(true));
    CHECK(wifi_driver_wiped());
    CHECK(wifi_driver_wiped_set(false) && !wifi_driver_wiped()); // a failed restore unmarks, so the next boot retries
    CHECK(wifi_driver_wiped_set(true));
    CHECK(wifi_cred_clear());
    CHECK(wifi_driver_wiped());

    secrets_and_defaults();
    saved_networks();

    if (failures) {
        return 1;
    }
    printf("wifi_cred_test: ok\n");
    return 0;
}
