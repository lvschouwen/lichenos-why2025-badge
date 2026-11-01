// The status LED service (#74): one 50 ms esp_timer evaluates led_pick() and writes the pin on change. Unread is
// re-read from lichen_notify only when its history changed (the generation counter is lock-free).
#include "led.h"
#include "led_cmd.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "lichen_notify.h"
#include "notify_os.h"
#include "nvs.h"

#include <stdatomic.h>

#define TICK_US     50000
#define ENABLED_KEY "status_led"

static char const *const TAG = "led";

static atomic_bool  s_enabled = true;
static atomic_bool  s_updating;
static atomic_int   s_override = LED_AUTO;
static atomic_int   s_pattern;
static esp_timer_handle_t s_timer;
static bool         s_started, s_lit;
static uint32_t     s_generation = UINT32_MAX;
static bool         s_unread;

static void drive(bool lit) {
    s_lit = lit;
    gpio_set_level(LED_GPIO, lit == (LED_ACTIVE_HIGH != 0));
}

static void tick(void *arg) {
    (void)arg;
    uint32_t gen = lichen_notify_generation();
    if (gen != s_generation) {
        s_generation = gen;
        s_unread     = lichen_notify_unread(NULL) > 0;
    }
    led_inputs_t const in = {
        .enabled  = atomic_load(&s_enabled),
        .dnd      = lichen_notify_dnd(),
        .unread   = s_unread,
        .updating = atomic_load(&s_updating),
        .override = (led_override_t)atomic_load(&s_override),
    };
    led_pattern_t p = led_pick(&in);
    atomic_store(&s_pattern, p);
    bool lit = led_level(p, (uint32_t)(esp_timer_get_time() / 1000));
    if (lit != s_lit) {
        drive(lit);
    }
}

void led_start(void) {
    if (s_started) {
        return;
    }
    nvs_handle_t nvs;
    uint8_t      on = 1;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, ENABLED_KEY, &on);
        nvs_close(nvs);
    }
    atomic_store(&s_enabled, on != 0);
    gpio_config_t const cfg = {.pin_bit_mask = 1ULL << LED_GPIO, .mode = GPIO_MODE_OUTPUT};
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d not configured", LED_GPIO);
        return;
    }
    drive(false);
    esp_timer_create_args_t const args = {.callback = tick, .name = "led"};
    if (esp_timer_create(&args, &s_timer) != ESP_OK || esp_timer_start_periodic(s_timer, TICK_US) != ESP_OK) {
        ESP_LOGE(TAG, "timer not started");
        return;
    }
    s_started = true;
    led_register_console();
}

bool led_enabled(void) {
    return atomic_load(&s_enabled);
}

void led_set_enabled(bool on) {
    atomic_store(&s_enabled, on);
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        esp_err_t err = on ? nvs_erase_key(nvs, ENABLED_KEY) : nvs_set_u8(nvs, ENABLED_KEY, 0); // on is the default
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
            nvs_commit(nvs);
        } else {
            ESP_LOGE(TAG, "setting not saved: %s", esp_err_to_name(err));
        }
        nvs_close(nvs);
    }
}

void led_set_updating(bool on) {
    atomic_store(&s_updating, on);
}

void led_set_override(led_override_t o) {
    atomic_store(&s_override, o);
}

led_pattern_t led_current(void) {
    return (led_pattern_t)atomic_load(&s_pattern);
}
