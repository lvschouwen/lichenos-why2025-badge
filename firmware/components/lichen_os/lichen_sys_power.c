// lichen_sys_power.h's last step (#65, #281): the badge down to deep sleep. Moved here from main's power hook so the
// thermal watchdog, an OS service, can switch the badge off without the shell (no display) and without linking upward.
#include "lichen_sys_power.h"

#include "c6link.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "fw_console.h"
#include "haptics.h"

void lichen_sys_power_off_now(void) {
    system_restart_note("power off");
    haptics_stop();
    lichen_sys_power_screen_off();
    c6link_hold_in_reset();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    ESP_LOGI("power", "power off: deep sleep, no wake source");
    esp_deep_sleep_start();
}
