// The vibration service (#68). One esp_timer ticks every 10 ms while a pattern plays and writes the PWM duty on
// change; nothing runs otherwise. Notifications reach it through lichen_notify's feedback hook, which the shell
// calls only for a toast that is allowed to give feedback (not do-not-disturb, not silent, within the source's rate).
#include "haptics.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "haptics_cmd.h"
#include "lichen_haptics.h"
#include "notify_os.h"
#include "nvs.h"

#include <stdatomic.h>

#define TICK_US      10000
#define PWM_HZ       20000 // above hearing; the motor smooths it
#define ENABLED_KEY  "haptics"
#define STRENGTH_KEY "haptic_str"
#define LEDC_TIMER   LEDC_TIMER_3
#define LEDC_CHANNEL LEDC_CHANNEL_7

static char const *const TAG = "haptics";

static atomic_bool       s_enabled = true;
static atomic_bool       s_updating;
static atomic_int        s_strength = HP_HIGH;
static atomic_int        s_pattern;
static atomic_llong      s_start_us;
static atomic_bool       s_playing;
static esp_timer_handle_t s_timer;
static bool              s_ready, s_on; // s_on: timer callback only
static hp_gate_t         s_gate;        // the notification hook runs on the LVGL task only

_Static_assert((int)LICHEN_HAPTIC_ALERT == (int)HP_ALERT && (int)LICHEN_HAPTIC_TICK == (int)HP_TICK &&
                   (int)LICHEN_HAPTIC_NOTIFY == (int)HP_NOTIFY && (int)LICHEN_HAPTIC_HIGH == (int)HP_HIGH,
               "public enums mirror the pure ones");

static void drive(uint32_t duty) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
}

static void motor(bool on) {
    s_on = on;
    drive(on ? hp_duty((hp_strength_t)atomic_load(&s_strength)) : 0);
}

static void tick(void *arg) {
    (void)arg;
    hp_pattern_t p = (hp_pattern_t)atomic_load(&s_pattern);
    uint32_t     t = (uint32_t)((esp_timer_get_time() - atomic_load(&s_start_us)) / 1000);
    bool         on = atomic_load(&s_playing) && !hp_done(p, t) && hp_on(p, t);
    if (on != s_on) {
        motor(on);
    }
    if (!atomic_load(&s_playing) || hp_done(p, t)) {
        atomic_store(&s_playing, false);
        esp_timer_stop(s_timer);
        if (s_on) {
            motor(false);
        }
    }
}

static void begin(hp_pattern_t p) {
    if (!s_ready) {
        return;
    }
    esp_timer_stop(s_timer); // a running buzz is replaced; fails harmlessly when idle
    atomic_store(&s_pattern, p);
    atomic_store(&s_start_us, esp_timer_get_time());
    atomic_store(&s_playing, true);
    motor(true); // the first step starts now, not after a tick
    if (esp_timer_start_periodic(s_timer, TICK_US) != ESP_OK) {
        atomic_store(&s_playing, false);
        motor(false);
    }
}

void haptics_play(hp_pattern_t p) {
    if (hp_allowed(atomic_load(&s_enabled), atomic_load(&s_updating))) {
        begin(p);
    }
}

void haptics_force(hp_pattern_t p) {
    if (atomic_load(&s_enabled)) {
        begin(p);
    }
}

void haptics_stop(void) {
    atomic_store(&s_playing, false);
    if (s_ready) {
        esp_timer_stop(s_timer);
        motor(false);
    }
}

void haptics_set_updating(bool on) {
    atomic_store(&s_updating, on);
    if (on) {
        haptics_stop();
    }
}

bool haptics_playing(void) {
    return atomic_load(&s_playing);
}

// lichen_notify's hook: one buzz per allowed toast, at most one per HP_NOTIFY_GAP_MS
static void on_notice(ln_level_t level) {
    if (hp_gate_take(&s_gate, (uint32_t)(esp_timer_get_time() / 1000), HP_NOTIFY_GAP_MS)) {
        haptics_play(hp_for_notice((uint8_t)level));
    }
}

static bool pwm_init(void) {
    gpio_set_level(HAPTICS_GPIO, 0); // level first: the pin never glitches high on its way to LEDC
    gpio_config_t const pin = {.pin_bit_mask = 1ULL << HAPTICS_GPIO, .mode = GPIO_MODE_OUTPUT};
    ledc_timer_config_t const timer = {.speed_mode      = LEDC_LOW_SPEED_MODE,
                                       .duty_resolution = LEDC_TIMER_8_BIT,
                                       .timer_num       = LEDC_TIMER,
                                       .freq_hz         = PWM_HZ,
                                       .clk_cfg         = LEDC_AUTO_CLK};
    ledc_channel_config_t const ch = {.gpio_num   = HAPTICS_GPIO,
                                      .speed_mode = LEDC_LOW_SPEED_MODE,
                                      .channel    = LEDC_CHANNEL,
                                      .timer_sel  = LEDC_TIMER,
                                      .duty       = 0,
                                      .hpoint     = 0};
    return gpio_config(&pin) == ESP_OK && ledc_timer_config(&timer) == ESP_OK && ledc_channel_config(&ch) == ESP_OK;
}

static void load_settings(void) {
    nvs_handle_t nvs;
    uint8_t      on = 1, str = HP_HIGH;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, ENABLED_KEY, &on);
        nvs_get_u8(nvs, STRENGTH_KEY, &str);
        nvs_close(nvs);
    }
    atomic_store(&s_enabled, on != 0);
    atomic_store(&s_strength, str < HP_STRENGTH_COUNT ? str : HP_HIGH);
}

void haptics_start(void) {
    if (s_ready) {
        return;
    }
    if (!pwm_init()) {
        ESP_LOGE(TAG, "GPIO%d PWM not configured, no vibration", HAPTICS_GPIO);
        return;
    }
    load_settings();
    esp_timer_create_args_t const args = {.callback = tick, .name = "haptics"};
    if (esp_timer_create(&args, &s_timer) != ESP_OK) {
        ESP_LOGE(TAG, "timer not created, no vibration");
        return;
    }
    s_ready = true;
    ln_feedback_t const fb = {.vibrate = on_notice}; // sound (#67) and led members are not used yet
    lichen_notify_set_feedback(&fb);
    haptics_register_console();
}

// The person's choices: a default value is stored as an absent key
static void save_u8(char const *key, uint8_t v, uint8_t dflt) {
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    esp_err_t err = v == dflt ? nvs_erase_key(nvs, key) : nvs_set_u8(nvs, key, v);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_commit(nvs);
    } else {
        ESP_LOGE(TAG, "setting not saved: %s", esp_err_to_name(err));
    }
    nvs_close(nvs);
}

bool lichen_haptics_enabled(void) {
    return atomic_load(&s_enabled);
}

void lichen_haptics_set_enabled(bool on) {
    atomic_store(&s_enabled, on);
    if (!on) {
        haptics_stop();
    }
    save_u8(ENABLED_KEY, on, 1);
}

lichen_haptic_strength_t lichen_haptics_strength(void) {
    return (lichen_haptic_strength_t)atomic_load(&s_strength);
}

void lichen_haptics_set_strength(lichen_haptic_strength_t s) {
    if ((int)s < 0 || (int)s >= HP_STRENGTH_COUNT) {
        return;
    }
    atomic_store(&s_strength, (int)s);
    save_u8(STRENGTH_KEY, (uint8_t)s, HP_HIGH);
}

void lichen_haptics_play(lichen_haptic_t pattern) {
    if ((int)pattern >= 0 && (int)pattern < HP_COUNT) {
        haptics_play((hp_pattern_t)pattern);
    }
}
