#pragma once
// What the update server needs from the OS above it (#245): the settings (Settings › Updates over Wi-Fi, its auto-off,
// the token's NVS copy, all badge_fw keys as before) and the OS's Wi-Fi switch. lichen_os binds it in start_nvs, before
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
    // Copies the value (or def) into out, always NUL-terminated
    void (*get_str)(char const *key, char *out, size_t cap, char const *def);
    bool (*set_str)(char const *key, char const *value);
    // The OS's Wi-Fi switch (lichen_radio, airplane mode included) is on: the server closes when it goes off
    bool (*wifi_on)(void);
} ota_os_t;

// Copied; NULL unbinds. Any member may be NULL: it then does the default named below.
void ota_os_bind(ota_os_t const *os);

// The bound calls. Defaults: a read gives def, a write saves nothing (false), Wi-Fi counts as off.
uint8_t ota_os_get_u8(char const *key, uint8_t def);
bool    ota_os_set_u8(char const *key, uint8_t value);
void    ota_os_get_str(char const *key, char *out, size_t cap, char const *def);
bool    ota_os_set_str(char const *key, char const *value);
bool    ota_os_wifi_on(void);

#ifdef __cplusplus
}
#endif
