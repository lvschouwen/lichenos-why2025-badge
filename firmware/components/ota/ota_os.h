#pragma once
// What the update server needs from the OS above it (#245): the settings (Settings › Updates over Wi-Fi, its auto-off,
// the token's NVS copy as a secret (#241)) and the OS's Wi-Fi switch. lichen_os binds it in start_nvs, before
// ota_server_start: ota (L3) includes no gate header. Pure (no ESP headers), host-tested in host_tests/ota_os_test.c.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t (*get_u8)(char const *key, uint8_t def);
    bool (*set_u8)(char const *key, uint8_t value);
    // The secrets class (#241, the token's NVS copy): false when missing or unreadable, out then empty
    bool (*secret_get)(char const *key, char *out, size_t cap);
    bool (*secret_set)(char const *key, char const *value);
    // The OS's Wi-Fi switch (lichen_radio, airplane mode included) is on: the server closes when it goes off
    bool (*wifi_on)(void);
    // The trial's Wi-Fi rule (#411): the person's saved switch, whatever a hold does (lichen_radio_switch_get), and
    // whether the system holds Wi-Fi off for now (a thermal hold, lichen_sys_radio_held_by)
    bool (*wifi_switch_on)(void);
    bool (*wifi_held)(void);
} ota_os_t;

// Copied; NULL unbinds. Any member may be NULL: it then does the default named below.
void ota_os_bind(ota_os_t const *os);

// The bound calls. Defaults: a read finds nothing (false, out emptied), a write saves nothing (false), Wi-Fi counts as off;
// for the trial the cautious ones: the switch counts as on (the Wi-Fi rule applies) and nothing holds it.
uint8_t ota_os_get_u8(char const *key, uint8_t def);
bool    ota_os_set_u8(char const *key, uint8_t value);
bool    ota_os_secret_get(char const *key, char *out, size_t cap);
bool    ota_os_secret_set(char const *key, char const *value);
bool    ota_os_wifi_on(void);
bool    ota_os_wifi_switch_on(void);
bool    ota_os_wifi_held(void);

#ifdef __cplusplus
}
#endif
