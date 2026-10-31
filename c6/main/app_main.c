// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
/*
 * ESP-Hosted 3.x co-processor entry point for the WHY2025 badge C6.
 *
 * The esp_hosted component starts the co-processor itself; the badge only adds
 * the display (GPIO15) and keyboard (GPIO10) backlight PWM, which hang off the C6.
 */

#include "backlight_cmd.h"
#include "lora_service.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

// Both backlights share one LEDC timer; 25 kHz keeps the PWM out of hearing range
#define BL_TIMER      LEDC_TIMER_0
#define BL_RESOLUTION 8
#define BL_FREQ_HZ    25000

// The screen's AP3032 boost driver flickers on battery at 10%: its on-time per PWM period gets too short to ride out
// ripple on the IP5306's 5 V (#39). The keyboard LEDs are driven directly and are fine at 10%.
enum { BL_DISPLAY, BL_KEYBOARD, BL_COUNT };
static struct {
	int gpio;
	ledc_channel_t channel;
	int boot_percent;
} const backlights[BL_COUNT] = {
	[BL_DISPLAY]  = { .gpio = 15, .channel = LEDC_CHANNEL_0, .boot_percent = 30 },
	[BL_KEYBOARD] = { .gpio = 10, .channel = LEDC_CHANNEL_1, .boot_percent = 10 },
};

static void backlight_set(int which, int percent)
{
	ledc_channel_t channel = backlights[which].channel;
	ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, backlight_duty(percent, BL_RESOLUTION)));
	ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, channel));
}

static void backlights_start(void)
{
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

// The P4 sets the brightness with "BL <display%> <keyboard%>" lines on our console UART (its UART1, the line it
// also flashes us over); everything else arriving there is ignored
static void backlight_command_task(void *arg)
{
	char line[32];
	size_t len = 0;
	for (;;) {
		uint8_t c;
		if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) {
			continue;
		}
		if (c == '\n' || c == '\r') {
			line[len] = '\0';
			int display, keyboard;
			if (len && backlight_cmd_parse(line, &display, &keyboard)) {
				backlight_set(BL_DISPLAY, display);
				backlight_set(BL_KEYBOARD, keyboard);
				ESP_LOGI("backlight", "display %d%%, keyboard %d%%", display, keyboard);
			}
			len = 0;
		} else if (len < sizeof(line) - 1) {
			line[len++] = (char)c;
		} else {
			len = 0; // overlong garbage; start over at the next newline
		}
	}
}

void app_main(void)
{
	backlights_start();

	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	ESP_ERROR_CHECK(esp_event_loop_create_default());

	// Waits for the P4 to configure it; a failure leaves Wi-Fi and BLE running
	lora_service_start();

	// Reading needs the UART driver; the console keeps writing through it
	if (uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0) == ESP_OK) {
		xTaskCreate(backlight_command_task, "backlight_cmd", 2560, NULL, 1, NULL);
	}
}
