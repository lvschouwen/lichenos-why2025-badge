#include "c6_flasher.h"

#include "c6_firmware.h"
#include "c6_paths.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp32_port.h"
#include "esp_loader.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>

#include <inttypes.h>

static char const *TAG = "c6_flasher";

static bool image_matches(esp_loader_t *loader, partition_attr_t const *image) {
    return esp_loader_flash_verify_known_md5(loader, image->addr, image->size, image->md5) == ESP_LOADER_SUCCESS;
}

// Flash one image unless the C6 already holds it. Returns false if flashing was needed and failed.
static bool flash_if_changed(esp_loader_t *loader, partition_attr_t const *image, char const *what, bool matches) {
    if (matches) {
        ESP_LOGW(TAG, "%s MD5 match, skipping...", what);
        return true;
    }

    ESP_LOGW(TAG, "%s MD5 mismatch, flashing...", what);
    if (flash_binary(loader, image->fp, image->size, image->addr) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "Flashing %s failed", what);
        return false;
    }
    return true;
}

// otadata may still select an OTA slot the flasher never writes (ota_1 after an OTA update), so the C6 would keep
// booting the old image from there. Erasing it makes the bootloader fall back to the app at 0x10000. Done before
// flashing so an interrupted flash still retries: the MD5 mismatch persists until the images are written.
static bool reset_ota_selection(esp_loader_t *loader, c6_binaries_t *bin) {
    uint32_t offset;
    uint32_t size;

    if (!c6_binaries_otadata(bin, &offset, &size)) {
        ESP_LOGW(TAG, "No otadata partition in the partition table, nothing to erase");
        return true;
    }

    ESP_LOGW(TAG, "Erasing otadata at 0x%" PRIx32 " (%" PRIu32 " bytes)", offset, size);
    if (esp_loader_flash_erase_region(loader, offset, size) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "Erasing otadata failed");
        return false;
    }
    return true;
}

// force: write all three images even when the C6 already holds them ("fw c6 reflash")
static bool flash_images(esp_loader_t *loader, c6_binaries_t *bin, bool force) {
    bool boot_ok = !force && image_matches(loader, &bin->boot);
    bool part_ok = !force && image_matches(loader, &bin->part);
    bool app_ok  = !force && image_matches(loader, &bin->app);

    if ((!part_ok || !app_ok) && !reset_ota_selection(loader, bin)) {
        return false;
    }

    // Stop at the first failure; the next boot sees the mismatch and tries again
    return flash_if_changed(loader, &bin->boot, "Bootloader", boot_ok) &&
           flash_if_changed(loader, &bin->part, "Partition table", part_ok) &&
           flash_if_changed(loader, &bin->app, "Application", app_ok);
}

esp_err_t flash_c6_if_needed(bool force) {
    c6_binaries_t bin;

    if (!get_c6_binaries(&bin)) {
        ESP_LOGE(TAG, "C6 image in " C6_IMAGE_DIR " is missing or invalid, leaving the C6 alone");
        free_c6_binaries(&bin);
        return ESP_ERR_NOT_FOUND;
    }

    esp32_port_t port = {
        .port.ops    = &esp32_uart_ops,
        .baud_rate   = 115200,
        .uart_port   = UART_NUM_1,
        .uart_rx_pin = GPIO_NUM_15,
        .uart_tx_pin = GPIO_NUM_16,
        .reset_pin   = GPIO_NUM_12,
        .boot_pin    = GPIO_NUM_13,
    };
    esp_loader_t loader;

    if (esp_loader_init_serial(&loader, &port.port) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "serial initialization failed.");
        free_c6_binaries(&bin);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    if (connect_to_target_with_stub(&loader, 115200, 3686400) != ESP_LOADER_SUCCESS) {
        // connect_to_target_with_stub() already reset the C6 back into its application
        ret = ESP_ERR_TIMEOUT;
        goto out;
    }

    target_chip_t target = esp_loader_get_target(&loader);
    ESP_LOGW(TAG, "Target = %d", target);
    if (target != ESP32C6_CHIP) {
        ESP_LOGE(TAG, "wrong target, expecting ESP32C6_CHIP");
        ret = ESP_ERR_INVALID_RESPONSE;
    } else if (!flash_images(&loader, &bin, force)) {
        ret = ESP_FAIL;
    }

    ESP_LOGW(TAG, "Resetting C6!");
    esp_loader_reset_target(&loader);

out:
    ESP_LOGW(TAG, "Done (%s)", esp_err_to_name(ret));
    free_c6_binaries(&bin);
    esp_loader_deinit(&loader);
    return ret;
}

void c6_log_console(uint32_t duration_ms) {
    uart_config_t const config = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // At runtime the backlight code has the UART open for its commands to the C6: read through its driver, and
    // whatever the C6 printed since is still in the receive buffer (#56)
    bool open_here = !uart_is_driver_installed(UART_NUM_1);
    if (open_here) {
        if (uart_driver_install(UART_NUM_1, 2048, 0, 0, NULL, 0) != ESP_OK) {
            ESP_LOGE(TAG, "Can't open the C6 console");
            return;
        }
        uart_param_config(UART_NUM_1, &config);
        uart_set_pin(UART_NUM_1, GPIO_NUM_16, GPIO_NUM_15, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }

    ESP_LOGW(TAG, "C6 console for %" PRIu32 " ms:", duration_ms);
    char    line[160];
    size_t  len      = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        uint8_t buf[64];
        int     n = uart_read_bytes(UART_NUM_1, buf, sizeof(buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n' || len == sizeof(line) - 1) {
                line[len] = '\0';
                if (len) {
                    ESP_LOGW("c6", "%s", line);
                }
                len = 0;
            } else if (c >= 0x20 && c < 0x7F) {
                line[len++] = c; // drop control and escape bytes, the console may be mid-boot noise
            }
        }
    }
    if (len) {
        line[len] = '\0';
        ESP_LOGW("c6", "%s", line);
    }
    if (open_here) {
        uart_driver_delete(UART_NUM_1);
    }
}
