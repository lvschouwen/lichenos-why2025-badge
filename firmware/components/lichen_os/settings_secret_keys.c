#include "settings_secret.h"

#include "wifi_saved.h" // WIFI_SAVED_MAX: one password slot per saved network

#include <string.h>

#define OS_NS "badge_fw" // fw_console.h's FW_NVS_NAMESPACE; this file stays free of ESP headers

// A new secret goes on this list; the tests then prove it stays out of the backup file and the reset's keep list
static char const *const secret_keys[] = {
    "wifi_pass",                                         // the Wi-Fi pair in use (#66)
    "wifi_p0", "wifi_p1", "wifi_p2", "wifi_p3", "wifi_p4", // the saved networks (#160, WIFI_SAVED_MAX)
};
_Static_assert(sizeof(secret_keys) / sizeof(secret_keys[0]) == WIFI_SAVED_MAX + 1,
               "one secret per saved network plus the pair in use: a new WIFI_SAVED_MAX needs its wifi_pN here");

size_t settings_secret_count(void) {
    return sizeof(secret_keys) / sizeof(secret_keys[0]);
}

char const *settings_secret_at(size_t index) {
    return index < settings_secret_count() ? secret_keys[index] : NULL;
}

bool settings_key_secret(char const *ns, char const *key) {
    if (!ns || !key || strcmp(ns, OS_NS) != 0) {
        return false;
    }
    for (size_t i = 0; i < settings_secret_count(); i++) {
        if (strcmp(secret_keys[i], key) == 0) {
            return true;
        }
    }
    return false;
}
