#include "backlight.h"
#include "backlight_level.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "fw_console.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>

#define BL_UART    UART_NUM_1 // the C6's console UART; the C6 flasher and "fw c6log" use it too
#define BL_TX_PIN  16
#define BL_RX_PIN  15
#define BL_DEFAULT_DISPLAY  30 // what the C6 firmware starts with
#define BL_DEFAULT_KEYBOARD 10

static char const TAG[] = "backlight";

static bool send(uint8_t display, uint8_t keyboard) {
    if (!uart_is_driver_installed(BL_UART)) {
        uart_config_t const cfg = {
            .baud_rate  = 115200,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        // 2 KiB receive buffer: "fw c6log" reads the C6's console through this driver
        if (uart_driver_install(BL_UART, 2048, 0, 0, NULL, 0) != ESP_OK) {
            ESP_LOGE(TAG, "UART to the C6 not available");
            return false;
        }
        uart_param_config(BL_UART, &cfg);
        uart_set_pin(BL_UART, BL_TX_PIN, BL_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    char cmd[24];
    int  len = snprintf(cmd, sizeof(cmd), "\nBL %u %u\n", display, keyboard); // leading \n ends a partial line
    return uart_write_bytes(BL_UART, cmd, (size_t)len) == len;
}

static void save(uint8_t display, uint8_t keyboard) {
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "bl_disp", display);
        nvs_set_u8(nvs, "bl_kbd", keyboard);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void backlight_set(uint8_t display, uint8_t keyboard) {
    send(backlight_display_level(display), backlight_keyboard_level(keyboard));
}

void backlight_set_saved(uint8_t display, uint8_t keyboard) {
    display  = backlight_display_level(display);
    keyboard = backlight_keyboard_level(keyboard);
    send(display, keyboard);
    save(display, keyboard);
}

void backlight_get(uint8_t *display, uint8_t *keyboard) {
    uint8_t      d = BL_DEFAULT_DISPLAY, k = BL_DEFAULT_KEYBOARD;
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "bl_disp", &d);
        nvs_get_u8(nvs, "bl_kbd", &k);
        nvs_close(nvs);
    }
    // an older build could save a screen level of 0: that booted to a black screen
    if (display) {
        *display = backlight_display_level(d);
    }
    if (keyboard) {
        *keyboard = backlight_keyboard_level(k);
    }
}

static void cmd_bl(char const *args) {
    char         *end;
    unsigned long d = strtoul(args, &end, 10);
    unsigned long k = strtoul(end, &end, 10);
    if (!*args || *end || d > 100 || k > 100) {
        console_reply("bl <display %d-100> <keyboard 0-100>", BACKLIGHT_DISPLAY_MIN);
        return;
    }
    backlight_set_saved((uint8_t)d, (uint8_t)k);
    uint8_t display, keyboard;
    backlight_get(&display, &keyboard);
    console_reply("backlight %u %u", display, keyboard);
}

void backlight_start(void) {
    console_register("bl", "bl <display> <keyboard> (backlight 0-100, screen at least 5)", cmd_bl);
    uint8_t display, keyboard;
    backlight_get(&display, &keyboard);
    send(display, keyboard);
}
