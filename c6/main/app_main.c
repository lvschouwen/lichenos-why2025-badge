// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
/*
 * ESP-Hosted 3.x co-processor entry point for the WHY2025 badge C6.
 *
 * The esp_hosted component starts the co-processor itself; the badge only adds
 * the display (GPIO15) and keyboard (GPIO10) backlight PWM, which hang off the C6.
 */

#include "backlight.h"
#include "backlight_cmd.h"
#include "lora_service.h"
#include "reg_c6.h"
#include "driver/uart.h"
#include "esp_bt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

uint8_t c6_bt_status(void)
{
	return (uint8_t)esp_bt_controller_get_status();
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
				bool const ok = backlight_apply(BL_DISPLAY, display) && backlight_apply(BL_KEYBOARD, keyboard);
				ESP_LOGI("backlight", "display %d%%, keyboard %d%%%s", display, keyboard, ok ? "" : ": LEDC refused");
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
	backlight_start();

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
