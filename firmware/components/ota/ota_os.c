// ota_os.h: the update server's bound settings and Wi-Fi switch, with a defined default for every unbound call (#245)
#include "ota_os.h"

#include <string.h>

static ota_os_t s_os; // bound once in start_nvs, read by the OTA task and the console afterwards

void ota_os_bind(ota_os_t const *os) {
    if (os) {
        s_os = *os;
    } else {
        memset(&s_os, 0, sizeof(s_os));
    }
}

uint8_t ota_os_get_u8(char const *key, uint8_t def) {
    return s_os.get_u8 ? s_os.get_u8(key, def) : def;
}

bool ota_os_set_u8(char const *key, uint8_t value) {
    return s_os.set_u8 && s_os.set_u8(key, value);
}

bool ota_os_secret_get(char const *key, char *out, size_t cap) {
    if (s_os.secret_get) {
        return s_os.secret_get(key, out, cap);
    }
    if (cap) {
        out[0] = '\0';
    }
    return false;
}

bool ota_os_secret_set(char const *key, char const *value) {
    return s_os.secret_set && s_os.secret_set(key, value);
}

bool ota_os_wifi_on(void) {
    return s_os.wifi_on && s_os.wifi_on();
}

bool ota_os_wifi_switch_on(void) {
    return !s_os.wifi_switch_on || s_os.wifi_switch_on();
}

bool ota_os_wifi_held(void) {
    return s_os.wifi_held && s_os.wifi_held();
}
