#pragma once

#include "esp_err.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Make sure the C6 runs the image in C6_IMAGE_DIR, flashing what differs (everything when force is set).
 *
 * @return ESP_OK                    the C6 runs the requested firmware
 *         ESP_ERR_TIMEOUT           the C6 bootloader could not be reached; the C6 was not touched
 *         ESP_ERR_NOT_FOUND         the firmware files are missing or invalid; the C6 was not touched
 *         ESP_ERR_INVALID_RESPONSE  the target is not an ESP32-C6
 *         ESP_ERR_INVALID_STATE     UART1 stayed lent to "fw c6log" (#250); the C6 was not touched
 *         ESP_FAIL                  flashing failed, the C6 may not boot until it is flashed successfully
 *
 * Borrows UART1 from the backlight (backlight_uart_lend) from the serial init to the deinit.
 */
esp_err_t flash_c6_if_needed(bool force);

/**
 * Copy what the C6 prints on its console (UART0, wired to the P4's UART1) into the BadgeVMS log for duration_ms.
 * Borrows UART1 from the backlight for the whole read (#250) and reads through its driver when it is installed; refuses
 * (console reply) while the flasher or another c6log has it.
 */
void c6_log_console(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
