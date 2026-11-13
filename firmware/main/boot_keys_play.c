// "fw boot keys" played at boot (#379): the armed combo pressed through keyboard_inject, as if held since power-on,
// and released after its hold by a one-shot esp_timer, so boot does not wait for it.
#include "boot_keys_play.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "keyboard.h"

static char const TAG[] = "boot_keys";

static boot_keys_plan_t   s_plan; // written before the timer starts, read by its one callback
static esp_timer_handle_t s_release;

static void release(void *arg) {
    (void)arg;
    for (uint8_t i = 0; i < s_plan.count; i++) {
        if (!keyboard_inject(s_plan.codes[i], false, false)) {
            ESP_LOGE(TAG, "release of key 0x%02x not sent", s_plan.codes[i]);
        }
    }
    ESP_LOGI(TAG, "released after %u ms", (unsigned)s_plan.hold_ms);
}

void boot_keys_play(boot_keys_t k) {
    s_plan = boot_keys_plan(k);
    if (s_plan.count == 0) {
        return;
    }
    esp_timer_create_args_t const args = {.callback = release, .name = "boot_keys"};
    if (esp_timer_create(&args, &s_release) != ESP_OK) {
        ESP_LOGE(TAG, "%s not held: no timer to release it", boot_keys_name(k));
        return;
    }
    // Not as_physical: the remote key lock (#190) must not drop what the bench asked for. Press and release both skip
    // it, so the gate never sees half a key.
    for (uint8_t i = 0; i < s_plan.count; i++) {
        if (!keyboard_inject(s_plan.codes[i], true, false)) {
            ESP_LOGE(TAG, "press of key 0x%02x not sent", s_plan.codes[i]);
        }
    }
    esp_timer_start_once(s_release, (uint64_t)s_plan.hold_ms * 1000);
    ESP_LOGI(TAG, "%s held for %u ms (fw boot keys, once)", boot_keys_name(k), (unsigned)s_plan.hold_ms);
}
