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

#define PWM_TIMER_BASE_CLK   LEDC_USE_XTAL_CLK
#define PWM_TIMER_RESOLUTION LEDC_TIMER_8_BIT
#define PWM_TIMER_FREQ_HZ    25000 // Set well above hearing frequency

// The screen's AP3032 boost driver flickers on battery at 10%: its on-time per PWM period gets too short to ride out
// ripple on the IP5306's 5 V (#39). The keyboard LEDs are driven directly and are fine at 10%.
#define DISPLAY_BACKLIGHT_PERCENT  30
#define KEYBOARD_BACKLIGHT_PERCENT 10

static void configure_pwm_timer(ledc_timer_t timer_num)
{
	ledc_timer_config_t timer_conf = {
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.duty_resolution = PWM_TIMER_RESOLUTION,
		.timer_num = timer_num,
		.freq_hz = PWM_TIMER_FREQ_HZ,
		.clk_cfg = PWM_TIMER_BASE_CLK,
	};
	ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));
}

static void configure_pwm(int gpio_num, ledc_channel_t channel, ledc_timer_t timer_num)
{
	ledc_channel_config_t channel_conf = {
		.gpio_num = gpio_num,
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.channel = channel,
		.intr_type = LEDC_INTR_DISABLE,
		.timer_sel = timer_num,
		.duty = 0,
		.hpoint = 0,
	};
	ESP_ERROR_CHECK(ledc_channel_config(&channel_conf));
}

/// @brief set the pwm value, limiting the duty to 0 or 10-80%
static void set_pwm_duty_cycle(ledc_channel_t channel, int duty_percentage)
{
	if (duty_percentage < 10 && duty_percentage != 0) {
		duty_percentage = 10;
	}
	if (duty_percentage > 80) {
		duty_percentage = 80;
	}
	uint32_t duty = (uint32_t)((1 << (PWM_TIMER_RESOLUTION)) * duty_percentage / 100);
	ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty));
	ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, channel));
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
				set_pwm_duty_cycle(LEDC_CHANNEL_0, display);
				set_pwm_duty_cycle(LEDC_CHANNEL_1, keyboard);
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
	// display (GPIO15) and keyboard (GPIO10) backlight share one PWM timer
	configure_pwm_timer(LEDC_TIMER_0);
	configure_pwm(15, LEDC_CHANNEL_0, LEDC_TIMER_0);
	set_pwm_duty_cycle(LEDC_CHANNEL_0, DISPLAY_BACKLIGHT_PERCENT);
	configure_pwm(10, LEDC_CHANNEL_1, LEDC_TIMER_0);
	set_pwm_duty_cycle(LEDC_CHANNEL_1, KEYBOARD_BACKLIGHT_PERCENT);

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
