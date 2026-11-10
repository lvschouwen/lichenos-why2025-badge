// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
#include "backlight.h"

#include "backlight_cmd.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Both backlights share one LEDC timer; 25 kHz keeps the PWM out of hearing range
#define BL_TIMER      LEDC_TIMER_0
#define BL_RESOLUTION 8
#define BL_FREQ_HZ    25000
// The duty register a read-back sees changes at the start of the next PWM period (40 us at 25 kHz): wait two
#define BL_SETTLE_US  (2 * 1000000 / BL_FREQ_HZ)

// The screen's AP3032 boost driver flickers on battery at 10%: its on-time per PWM period gets too short to ride out
// ripple on the IP5306's 5 V (#39). The keyboard LEDs are driven directly and are fine at 10%.
static struct {
	int gpio;
	ledc_channel_t channel;
	int boot_percent;
} const backlights[BL_COUNT] = {
	[BL_DISPLAY]  = { .gpio = 15, .channel = LEDC_CHANNEL_0, .boot_percent = 30 },
	[BL_KEYBOARD] = { .gpio = 10, .channel = LEDC_CHANNEL_1, .boot_percent = 10 },
};

static StaticSemaphore_t lock_buf;
static SemaphoreHandle_t lock;
static bool              at_boot = true;

void backlight_start(void)
{
	lock = xSemaphoreCreateMutexStatic(&lock_buf);
	ledc_timer_config_t const timer = {
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.timer_num = BL_TIMER,
		.duty_resolution = (ledc_timer_bit_t)BL_RESOLUTION,
		.freq_hz = BL_FREQ_HZ,
		.clk_cfg = LEDC_USE_XTAL_CLK,
	};
	ESP_ERROR_CHECK(ledc_timer_config(&timer));
	for (int i = 0; i < BL_COUNT; i++) {
		ledc_channel_config_t const ch = {
			.gpio_num = backlights[i].gpio,
			.speed_mode = LEDC_LOW_SPEED_MODE,
			.channel = backlights[i].channel,
			.timer_sel = BL_TIMER,
			.duty = backlight_duty(backlights[i].boot_percent, BL_RESOLUTION),
		};
		ESP_ERROR_CHECK(ledc_channel_config(&ch));
	}
}

bool backlight_apply(int which, int percent)
{
	if (which < 0 || which >= BL_COUNT || percent < 0 || percent > 100) {
		return false;
	}
	ledc_channel_t const channel = backlights[which].channel;
	xSemaphoreTake(lock, portMAX_DELAY);
	bool ok = ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, backlight_duty(percent, BL_RESOLUTION)) == ESP_OK &&
		  ledc_update_duty(LEDC_LOW_SPEED_MODE, channel) == ESP_OK;
	if (ok) {
		at_boot = false;
		esp_rom_delay_us(BL_SETTLE_US); // so a read-back right after shows the new duty, not the old one
	}
	xSemaphoreGive(lock);
	return ok;
}

bool backlight_applied(int which, uint8_t *percent)
{
	if (which < 0 || which >= BL_COUNT) {
		return false;
	}
	xSemaphoreTake(lock, portMAX_DELAY);
	uint32_t const duty = ledc_get_duty(LEDC_LOW_SPEED_MODE, backlights[which].channel);
	xSemaphoreGive(lock);
	if (duty == LEDC_ERR_DUTY) {
		return false;
	}
	*percent = (uint8_t)backlight_percent(duty, BL_RESOLUTION);
	return true;
}

uint32_t backlight_freq_hz(void)
{
	xSemaphoreTake(lock, portMAX_DELAY);
	uint32_t const hz = ledc_get_freq(LEDC_LOW_SPEED_MODE, BL_TIMER);
	xSemaphoreGive(lock);
	return hz;
}

bool backlight_at_boot(void)
{
	xSemaphoreTake(lock, portMAX_DELAY);
	bool const boot = at_boot;
	xSemaphoreGive(lock);
	return boot;
}
