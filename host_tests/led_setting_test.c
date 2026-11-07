// Host test: the status LED's saved switch goes through the settings binding, with the badge_fw key from before #235
#include "led_setting.h"

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

static int  stored = -1; // -1: key missing
static char last_key[32];
static uint8_t fake_get(char const *key, uint8_t def) {
    strcpy(last_key, key);
    return stored >= 0 ? (uint8_t)stored : def;
}
static bool fake_set(char const *key, uint8_t value) {
    strcpy(last_key, key);
    stored = value;
    return true;
}

int main(void) {
    CHECK(strcmp(LED_KEY_ENABLED, "status_led") == 0); // renaming it would reset every saved setting
    CHECK(led_setting_load(NULL));                      // not bound: on
    CHECK(led_setting_load(fake_get));                  // missing: on
    CHECK(strcmp(last_key, "status_led") == 0);
    CHECK(!led_setting_save(NULL, false));
    CHECK(led_setting_save(fake_set, false) && stored == 0 && !led_setting_load(fake_get));
    CHECK(led_setting_save(fake_set, true) && stored == 1 && led_setting_load(fake_get));
    if (failures) {
        return 1;
    }
    printf("led_setting_test: ok\n");
    return 0;
}
