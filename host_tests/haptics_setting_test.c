// Host test: the vibration setting goes through the settings binding, with the badge_fw keys from before #235
#include "haptics_setting.h"

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

static int stored_on = -1, stored_str = -1; // -1: key missing
static uint8_t fake_get(char const *key, uint8_t def) {
    int v = strcmp(key, "haptics") == 0 ? stored_on : strcmp(key, "haptic_str") == 0 ? stored_str : -1;
    return v >= 0 ? (uint8_t)v : def;
}
static bool fake_set(char const *key, uint8_t value) {
    if (strcmp(key, "haptics") == 0) {
        stored_on = value;
    } else if (strcmp(key, "haptic_str") == 0) {
        stored_str = value;
    } else {
        return false;
    }
    return true;
}

int main(void) {
    CHECK(strcmp(HAPTICS_KEY_ENABLED, "haptics") == 0);
    CHECK(strcmp(HAPTICS_KEY_STRENGTH, "haptic_str") == 0);
    bool          on  = false;
    hp_strength_t str = HP_LOW;
    haptics_setting_load(NULL, &on, &str); // not bound: the defaults
    CHECK(on && str == HP_HIGH);
    on = false;
    str = HP_LOW;
    haptics_setting_load(fake_get, &on, &str); // nothing saved: the defaults
    CHECK(on && str == HP_HIGH);

    CHECK(!haptics_setting_save_enabled(NULL, false) && !haptics_setting_save_strength(NULL, HP_LOW));
    CHECK(haptics_setting_save_enabled(fake_set, false) && haptics_setting_save_strength(fake_set, HP_LOW));
    haptics_setting_load(fake_get, &on, &str);
    CHECK(!on && str == HP_LOW);
    stored_str = 99; // out of range: high
    haptics_setting_load(fake_get, &on, &str);
    CHECK(str == HP_HIGH);
    if (failures) {
        return 1;
    }
    printf("haptics_setting_test: ok\n");
    return 0;
}
