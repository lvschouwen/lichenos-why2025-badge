// os_bind_lower (lichen_os.h, #245): lichen_os wires the layers below the gate together, so none of them includes a
// gate header or calls up. The settings for lichen_boot's "fw boot fail" flag, lichen_clock's source record and the
// update server (with the OS's Wi-Fi switch); lichen_boot's factory reset notices into notify; and notify's state and
// feedback down into the status LED and the motor. Only stores pointers. ESP-free, host-tested in
// host_tests/os_bind_test.c.
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

static bool wifi_on(void) {
    return lichen_radio_get(RADIO_WIFI);
}

// lichen_notify's vibrate feedback, fired by the shell on the LVGL task for a toast with feedback (as before #245)
static void vibrate(ln_level_t level) {
    haptics_notice((uint8_t)level);
}

static void factory_notice(char const *title, char const *body) {
    lichen_notify("lichen", LN_ERROR, title, body, 0);
}

static ota_os_t const ota_os = {
    .get_u8  = settings_get_u8,
    .set_u8  = settings_set_u8,
    .get_str = settings_get_str,
    .set_str = settings_set_str,
    .wifi_on = wifi_on,
};

void os_bind_lower(void) {
    boot_progress_bind_store(settings_get_u8, settings_set_u8);
    clock_bind_store(settings_get_u8, settings_set_u8);
    ota_os_bind(&ota_os);
    factory_reset_bind_notice(factory_notice);
    ln_state_hooks_t const led_hooks = {.dnd = led_set_dnd, .unread = led_set_unread};
    lichen_notify_set_state_hooks(&led_hooks);
    ln_feedback_t const fb = {.vibrate = vibrate}; // sound (#67) and led members are not used yet
    lichen_notify_set_feedback(&fb);
}
