// Host tests for lichen_os's os_bind.c (#245): the drivers below notify get its state and feedback from what lichen_os
// binds (the LED's dnd/unread setters, haptics_notice as the vibrate feedback, called with the toast's level), and the
// lower services get the settings, the Wi-Fi switch and the factory reset's notice. Everything around it is stubbed.
#include "lichen_os.h"

#include "boot_progress.h"
#include "factory_reset.h"
#include "haptics.h"
#include "led.h"
#include "lichen_clock.h"
#include "lichen_notify.h"
#include "lichen_radio.h"
#include "notify_bind.h"
#include "notify_os.h"
#include "ota_os.h"
#include "settings_store.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// ---- stubs: what os_bind_lower binds, and what the bound calls reach ----------------------------------------------

static ln_state_hooks_t s_hooks;
static ln_feedback_t    s_fb;
static int              s_fb_sets;
static uint8_t (*s_boot_get)(char const *, uint8_t);
static bool (*s_boot_set)(char const *, uint8_t);
static uint8_t (*s_clock_get)(char const *, uint8_t);
static bool (*s_clock_set)(char const *, uint8_t);
static void (*s_factory)(char const *, char const *);
static int  s_led_dnd = -1, s_led_unread = -1, s_notice = -1, s_notifies;
static char s_source[16], s_title[64];
static int  s_level = -1;
static bool s_wifi;

void lichen_notify_set_state_hooks(ln_state_hooks_t const *hooks) {
    s_hooks = *hooks;
}
void lichen_notify_set_feedback(ln_feedback_t const *fb) {
    s_fb = *fb;
    s_fb_sets++;
}
void led_set_dnd(bool on) {
    s_led_dnd = on;
}
void led_set_unread(bool any) {
    s_led_unread = any;
}
void haptics_notice(uint8_t level) {
    s_notice = level;
}
void boot_progress_bind_store(uint8_t (*get)(char const *key, uint8_t def), bool (*set)(char const *key, uint8_t value)) {
    s_boot_get = get;
    s_boot_set = set;
}
void clock_bind_store(uint8_t (*get)(char const *key, uint8_t def), bool (*set)(char const *key, uint8_t value)) {
    s_clock_get = get;
    s_clock_set = set;
}
void factory_reset_bind_notice(void (*error)(char const *title, char const *body)) {
    s_factory = error;
}
uint32_t lichen_notify(char const *source, ln_level_t level, char const *title, char const *body, uint32_t flags) {
    (void)body;
    (void)flags;
    snprintf(s_source, sizeof(s_source), "%s", source);
    snprintf(s_title, sizeof(s_title), "%s", title);
    s_level = (int)level;
    s_notifies++;
    return 1;
}
bool lichen_radio_get(radio_id_t id) {
    return id == RADIO_WIFI && s_wifi;
}
uint8_t settings_get_u8(char const *key, uint8_t def) {
    return strcmp(key, "ota_wifi") == 0 ? 0 : def;
}
bool settings_set_u8(char const *key, uint8_t value) {
    (void)key;
    (void)value;
    return true;
}
void settings_get_str(char const *key, char *out, size_t cap, char const *def) {
    (void)key;
    snprintf(out, cap, "%s", def);
}
bool settings_set_str(char const *key, char const *value) {
    (void)key;
    (void)value;
    return true;
}

// ---- tests ----------------------------------------------------------------------------------------------------------

static void led_gets_notify_state(void) {
    CHECK(s_hooks.dnd == led_set_dnd);
    CHECK(s_hooks.unread == led_set_unread);
    s_hooks.dnd(true);
    s_hooks.unread(true);
    CHECK(s_led_dnd == 1 && s_led_unread == 1);
    s_hooks.unread(false);
    CHECK(s_led_unread == 0);
}

static void haptics_is_the_vibrate_feedback(void) {
    CHECK(s_fb_sets == 1); // one owner sets the whole struct (notify_os.h)
    CHECK(s_fb.vibrate != NULL);
    CHECK(s_fb.sound == NULL && s_fb.led == NULL);
    s_fb.vibrate(LN_WARN);
    CHECK(s_notice == 1);
    s_fb.vibrate(LN_ERROR);
    CHECK(s_notice == 2);
}

static void lower_services_get_settings(void) {
    CHECK(s_boot_get == settings_get_u8 && s_boot_set == settings_set_u8);
    CHECK(s_clock_get == settings_get_u8 && s_clock_set == settings_set_u8);
    CHECK(ota_os_get_u8("ota_wifi", 1) == 0); // through settings_get_u8, not the unbound default
    s_wifi = true;
    CHECK(ota_os_wifi_on());
    s_wifi = false;
    CHECK(!ota_os_wifi_on());
}

static void factory_notice_is_an_os_error(void) {
    if (!s_factory) {
        CHECK(s_factory != NULL); // factory_reset_bind_notice was not called
        return;
    }
    s_factory("Factory reset could not fully erase settings", "body");
    CHECK(s_notifies == 1);
    CHECK(strcmp(s_source, "lichen") == 0 && s_level == LN_ERROR);
    CHECK(strcmp(s_title, "Factory reset could not fully erase settings") == 0);
}

int main(void) {
    os_bind_lower();
    led_gets_notify_state();
    haptics_is_the_vibrate_feedback();
    lower_services_get_settings();
    factory_notice_is_an_os_error();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("os_bind_test: all passed\n");
    return 0;
}
