// Host test: the settings' secrets class (#235): never in the backup file's key table, never on the factory reset's
// keep list (so a reset erases a secret and never carries one through RAM), and the class covers every Wi-Fi password
#include "factory_plan.h"
#include "settings_backup.h"
#include "settings_secret.h"
#include "wifi_saved.h"

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

static void class_members(void) {
    char const *const passwords[] = {"wifi_pass", "wifi_p0", "wifi_p1", "wifi_p2", "wifi_p3", "wifi_p4", "ota_token", "identity"};
    for (size_t i = 0; i < sizeof(passwords) / sizeof(passwords[0]); i++) {
        CHECK(settings_key_secret("badge_fw", passwords[i]));
    }
    CHECK(settings_secret_count() == sizeof(passwords) / sizeof(passwords[0]));
    CHECK(settings_secret_count() == WIFI_SAVED_MAX + 3); // one per saved network, the pair in use, the OTA token, the old identity
    CHECK(settings_secret_at(settings_secret_count()) == NULL);
    // The SSIDs are plain settings; another namespace's key of the same name is not ours
    CHECK(!settings_key_secret("badge_fw", "wifi_ssid") && !settings_key_secret("badge_fw", "wifi_s0"));
    CHECK(!settings_key_secret("badge_fw", "tz"));
    CHECK(!settings_key_secret("a.meshcore", "wifi_pass"));
    CHECK(!settings_key_secret(NULL, "wifi_pass") && !settings_key_secret("badge_fw", NULL));
}

static void never_exported(void) {
    for (size_t i = 0; i < settings_secret_count(); i++) {
        CHECK(sb_find("badge_fw", settings_secret_at(i)) == NULL);
    }
    for (size_t i = 0; i < sb_key_count(); i++) { // and the other way round, whatever the table grows into
        CHECK(!settings_key_secret(sb_key_at(i)->ns, sb_key_at(i)->key));
    }
    // A backup file that names a secret loads nothing for it
    sb_value_t  v[4];
    size_t      n    = 99;
    char const  file[] = "badge_fw.wifi_pass = hunter2hunter2\nbadge_fw.wifi_p0 = hunter2hunter2\n";
    sb_parse(file, strlen(file), v, 4, &n);
    CHECK(n == 0);
}

static void reset_erases(void) {
    for (size_t i = 0; i < settings_secret_count(); i++) {
        CHECK(factory_nvs_erases("badge_fw", settings_secret_at(i), FACTORY_SETTINGS));
        CHECK(factory_nvs_erases("badge_fw", settings_secret_at(i), FACTORY_ALL));
    }
}

int main(void) {
    class_members();
    never_exported();
    reset_erases();
    if (failures) {
        return 1;
    }
    printf("settings_secret_test: ok\n");
    return 0;
}
