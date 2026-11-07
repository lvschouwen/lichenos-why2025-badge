// The status LED service (#74): one 50 ms esp_timer evaluates led_pick() and writes the pin on change. Do-not-disturb
// and "anything unread" are pushed in by the notification service (led_set_dnd / led_set_unread, bound by lichen_os,
// #245): the LED never calls up into notify.
#include "led.h"
#include "led_cmd.h"
#include "led_setting.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fw_console.h"

#include <stdatomic.h>

#define TICK_US     50000

static char const *const TAG = "led";

static fw_get_u8_fn s_get;
static fw_set_u8_fn s_set;
static atomic_bool  s_enabled = true;
static atomic_bool  s_updating;
static atomic_int   s_override = LED_AUTO;
static atomic_int   s_pattern;
static atomic_bool  s_dnd;    // pushed by notify from any task, read by the tick
static atomic_bool  s_unread; // likewise
static esp_timer_handle_t s_timer;
static bool         s_started, s_lit;

static void drive(bool lit) {
    s_lit = lit;
    gpio_set_level(LED_GPIO, lit == (LED_ACTIVE_HIGH != 0));
}

static led_pattern_t pick_now(void) {
    led_inputs_t const in = {
        .enabled  = atomic_load(&s_enabled),
        .dnd      = atomic_load(&s_dnd),
        .unread   = atomic_load(&s_unread),
        .updating = atomic_load(&s_updating),
        .override = (led_override_t)atomic_load(&s_override),
    };
    return led_pick(&in);
}

static void tick(void *arg) {
    (void)arg;
    led_pattern_t p = pick_now();
    atomic_store(&s_pattern, p);
    bool lit = led_level(p, (uint32_t)(esp_timer_get_time() / 1000));
    if (lit != s_lit) {
        drive(lit);
    }
}

void led_bind_store(fw_get_u8_fn get, fw_set_u8_fn set) {
    s_get = get;
    s_set = set;
}

void led_start(void) {
    if (s_started) {
        return;
    }
    atomic_store(&s_enabled, led_setting_load(s_get));
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
    if (!led_setting_save(s_set, on)) {
        ESP_LOGE(TAG, "setting not saved");
    }
}

void led_set_dnd(bool on) {
    atomic_store(&s_dnd, on);
}

void led_set_unread(bool any) {
    atomic_store(&s_unread, any);
}

void led_set_updating(bool on) {
    atomic_store(&s_updating, on);
}

void led_set_override(led_override_t o) {
    atomic_store(&s_override, o);
    if (s_started) {
        atomic_store(&s_pattern, pick_now()); // "fw led" reads it back at once, not after the next 50 ms tick (#125)
    }
}

led_pattern_t led_current(void) {
    return (led_pattern_t)atomic_load(&s_pattern);
}
