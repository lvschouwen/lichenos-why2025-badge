// The vibration service (#68). One esp_timer ticks every 10 ms while a pattern plays and writes the PWM duty on
// change; nothing runs otherwise. Notifications reach it through haptics_notice, which lichen_os registers as
// lichen_notify's vibrate feedback (#245: the driver includes no notify header); the shell fires that only for a toast
// that is allowed to give feedback (not do-not-disturb, not silent, within the source's rate).
#include "haptics.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "haptics_cmd.h"
#include "haptics_setting.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdatomic.h>

#define TICK_US      10000
#define PWM_HZ       20000 // above hearing; the motor smooths it
#define LEDC_TIMER   LEDC_TIMER_3
#define LEDC_CHANNEL LEDC_CHANNEL_7

static char const *const TAG = "haptics";

static fw_get_u8_fn s_get;
static fw_set_u8_fn s_set;

void haptics_bind_store(fw_get_u8_fn get, fw_set_u8_fn set) {
    s_get = get;
    s_set = set;
}

static atomic_bool       s_enabled = true;
static atomic_bool       s_updating;
static atomic_int        s_strength = HP_HIGH;
static atomic_int        s_pattern;
static atomic_llong      s_start_us;
static atomic_bool       s_playing;
static esp_timer_handle_t s_timer;
static bool              s_ready;
// tick() runs on the esp_timer task, begin() and haptics_stop() on any core: without the lock a tick that decided the
// old buzz is over could clear "playing" and stop the timer right after begin() started the next one (#125)
static SemaphoreHandle_t s_lock;
static uint8_t           s_duty; // the duty last written
static hp_gate_t         s_gate;        // the notification hook runs on the LVGL task only

// lichen_os/lichen_haptics.c checks that the gate's enums mirror the pure ones (#221)

static void drive(uint32_t duty) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
}

static void motor(uint8_t duty) {
    s_duty = duty;
    drive(duty);
}

static void tick(void *arg) {
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    hp_pattern_t p = (hp_pattern_t)atomic_load(&s_pattern);
    uint32_t     t = (uint32_t)((esp_timer_get_time() - atomic_load(&s_start_us)) / 1000);
    hp_strength_t s    = (hp_strength_t)atomic_load(&s_strength);
    bool          live = atomic_load(&s_playing) && !hp_done(p, s, t);
    uint8_t       duty = live ? hp_duty_at(p, s, t) : 0;
    if (duty != s_duty) {
        motor(duty);
    }
    if (!live) {
        atomic_store(&s_playing, false);
        esp_timer_stop(s_timer);
    }
    xSemaphoreGive(s_lock);
}

static void begin(hp_pattern_t p) {
    if (!s_ready) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_timer_stop(s_timer); // a running buzz is replaced; fails harmlessly when idle
    atomic_store(&s_pattern, p);
    atomic_store(&s_start_us, esp_timer_get_time());
    atomic_store(&s_playing, true);
    motor(hp_duty_at(p, (hp_strength_t)atomic_load(&s_strength), 0)); // the kick starts now, not after a tick
    if (esp_timer_start_periodic(s_timer, TICK_US) != ESP_OK) {
        atomic_store(&s_playing, false);
        motor(0);
    }
    xSemaphoreGive(s_lock);
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
        xSemaphoreTake(s_lock, portMAX_DELAY);
        esp_timer_stop(s_timer);
        motor(0);
        xSemaphoreGive(s_lock);
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

// One buzz per allowed toast, at most one per HP_NOTIFY_GAP_MS. Before haptics_start worked nothing happens and the
// gap is not taken, as when the hook was only registered by a successful start.
void haptics_notice(uint8_t level) {
    if (!s_ready) {
        return;
    }
    if (hp_gate_take(&s_gate, (uint32_t)(esp_timer_get_time() / 1000), HP_NOTIFY_GAP_MS)) {
        haptics_play(hp_for_notice(level));
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
    bool          on;
    hp_strength_t str;
    haptics_setting_load(s_get, &on, &str);
    atomic_store(&s_enabled, on);
    atomic_store(&s_strength, str);
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
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "lock not created, no vibration");
        return;
    }
    esp_timer_create_args_t const args = {.callback = tick, .name = "haptics"};
    if (esp_timer_create(&args, &s_timer) != ESP_OK) {
        ESP_LOGE(TAG, "timer not created, no vibration");
        return;
    }
    s_ready = true;
    haptics_register_console();
}

bool haptics_enabled(void) {
    return atomic_load(&s_enabled);
}

void haptics_set_enabled(bool on) {
    atomic_store(&s_enabled, on);
    if (!on) {
        haptics_stop();
    }
    if (!haptics_setting_save_enabled(s_set, on)) {
        ESP_LOGE(TAG, "setting not saved");
    }
}

hp_strength_t haptics_strength(void) {
    return (hp_strength_t)atomic_load(&s_strength);
}

void haptics_set_strength(hp_strength_t s) {
    if ((int)s < 0 || (int)s >= HP_STRENGTH_COUNT) {
        return;
    }
    atomic_store(&s_strength, (int)s);
    if (!haptics_setting_save_strength(s_set, s)) {
        ESP_LOGE(TAG, "setting not saved");
    }
}
