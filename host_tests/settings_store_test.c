// Host test: settings_store.c itself (#235), compiled over the NVS stub: the plain calls refuse the secrets class, and
// settings_bind_net() hands lichen_net a store that keeps the Wi-Fi passwords on the secret side only
#include "backlight.h"
#include "fw_console.h"
#include "haptics.h"
#include "led.h"
#include "lichen_os.h"
#include "notify_os.h"
#include "nvs.h"
#include "sensors.h"
#include "settings_secret.h"
#include "settings_store.h"
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

// settings_bind_services() binds these lower layers; this test only needs them to link
void backlight_bind_store(backlight_get_u8_fn get, backlight_set_u8_fn set) {
    (void)get;
    (void)set;
}
void sensors_bind_store(uint8_t (*get)(char const *key, uint8_t def), bool (*set)(char const *key, uint8_t value),
                        bool (*erase)(char const *key)) {
    (void)get;
    (void)set;
    (void)erase;
}
void lichen_notify_bind_store(fw_get_u8_fn get, fw_set_u8_fn set) {
    (void)get;
    (void)set;
}
void led_bind_store(fw_get_u8_fn get, fw_set_u8_fn set) {
    (void)get;
    (void)set;
}
void haptics_bind_store(fw_get_u8_fn get, fw_set_u8_fn set) {
    (void)get;
    (void)set;
}
void system_restart_bind_store(fw_get_blob_fn get, fw_set_blob_fn set) {
    (void)get;
    (void)set;
}

static void plain_refuses_secrets(void) {
    char v[64];
    CHECK(settings_secret_set("wifi_pass", "hunter2hunter2"));
    settings_get_str("wifi_pass", v, sizeof(v), "default");
    CHECK(strcmp(v, "default") == 0); // stored, but the plain path reads it as missing
    CHECK(!settings_set_str("wifi_p0", "leakleakleak"));
    CHECK(!settings_secret_get("wifi_p0", v, sizeof(v))); // and nothing was written
    CHECK(settings_secret_get("wifi_pass", v, sizeof(v)) && strcmp(v, "hunter2hunter2") == 0);
    CHECK(settings_erase("wifi_pass") && !settings_secret_get("wifi_pass", v, sizeof(v)));
    // Plain keys work as before
    CHECK(settings_set_str("wifi_ssid", "homenet"));
    settings_get_str("wifi_ssid", v, sizeof(v), "");
    CHECK(strcmp(v, "homenet") == 0);
    CHECK(settings_erase("wifi_ssid"));
}

static void bound_net_store(void) {
    char s[33], p[64], v[64];
    settings_bind_net();
    CHECK(wifi_cred_save("homenet", "homepassword"));
    CHECK(wifi_cred_load(s, p) && strcmp(s, "homenet") == 0 && strcmp(p, "homepassword") == 0);
    settings_get_str("wifi_pass", v, sizeof(v), "-");
    CHECK(strcmp(v, "-") == 0);
    settings_get_str("wifi_p0", v, sizeof(v), "-");
    CHECK(strcmp(v, "-") == 0);
    settings_get_str("wifi_ssid", v, sizeof(v), ""); // the shell's Wi-Fi line still reads the name
    CHECK(strcmp(v, "homenet") == 0);
    CHECK(!wifi_driver_wiped() && wifi_driver_wiped_set(true) && wifi_driver_wiped());
    CHECK(wifi_cred_clear() && !wifi_cred_load(s, p));
}

int main(void) {
    plain_refuses_secrets();
    bound_net_store();
    if (failures) {
        return 1;
    }
    printf("settings_store_test: ok\n");
    return 0;
}
