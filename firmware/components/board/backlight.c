#include "backlight.h"
#include "backlight_level.h"

#include "display.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "nvs.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define BL_UART    UART_NUM_1 // the C6's console UART; the C6 flasher and "fw c6log" use it too
#define BL_TX_PIN  16
#define BL_RX_PIN  15
#define BL_FRAME_MS 20 // one 60 Hz frame is 17 ms
#define BL_DEFAULT_DISPLAY  30 // what the C6 firmware starts with
#define BL_DEFAULT_KEYBOARD 10

static char const TAG[] = "backlight";

static bool open_uart(void) {
    if (uart_is_driver_installed(BL_UART)) {
        return true;
    }
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
    return true;
}

// The levels as given, no clamp
static bool send_raw(unsigned display, unsigned keyboard) {
    char   cmd[24];
    size_t len = backlight_cmd_format(cmd, sizeof(cmd), display, keyboard);
    return len && open_uart() && uart_write_bytes(BL_UART, cmd, len) == (int)len;
}

// The last levels asked for (a slider included, not only the saved ones): a C6 reset brings back its 30/10 defaults and
// backlight_reapply() sends these again (#14). The restart blank (send_raw 0 0) is not one of them.
static portMUX_TYPE applied_lock = portMUX_INITIALIZER_UNLOCKED;
static bool         applied_known;
static uint8_t      applied_display, applied_keyboard;

static bool send(uint8_t display, uint8_t keyboard) {
    portENTER_CRITICAL(&applied_lock);
    applied_known    = true;
    applied_display  = display;
    applied_keyboard = keyboard;
    portEXIT_CRITICAL(&applied_lock);
    return send_raw(display, keyboard);
}

void backlight_reapply(void) {
    uint8_t display, keyboard;
    portENTER_CRITICAL(&applied_lock);
    bool known = applied_known;
    display    = applied_display;
    keyboard   = applied_keyboard;
    portEXIT_CRITICAL(&applied_lock);
    if (!known) {
        backlight_get(&display, &keyboard);
    }
    if (!send_raw(display, keyboard)) {
        ESP_LOGW(TAG, "backlight not re-sent to the C6");
    }
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

// The person's levels (backlight_set*, the saved ones at start): what backlight_resume() goes back to after a dim (#43)
static uint8_t     user_display = BL_DEFAULT_DISPLAY, user_keyboard = BL_DEFAULT_KEYBOARD;
static atomic_uint user_gen;

static bool send_user(uint8_t display, uint8_t keyboard, bool count) {
    portENTER_CRITICAL(&applied_lock);
    user_display  = display;
    user_keyboard = keyboard;
    portEXIT_CRITICAL(&applied_lock);
    if (count) {
        atomic_fetch_add(&user_gen, 1);
    }
    return send(display, keyboard);
}

uint32_t backlight_user_generation(void) {
    return atomic_load(&user_gen);
}

void backlight_idle(uint8_t display, uint8_t keyboard) {
    send(display > 100 ? 100 : display, backlight_keyboard_level(keyboard));
}

void backlight_resume(void) {
    portENTER_CRITICAL(&applied_lock);
    uint8_t display  = user_display;
    uint8_t keyboard = user_keyboard;
    portEXIT_CRITICAL(&applied_lock);
    send(display, keyboard);
}

void backlight_set(uint8_t display, uint8_t keyboard) {
    send_user(backlight_display_level(display), backlight_keyboard_level(keyboard), true);
}

void backlight_set_saved(uint8_t display, uint8_t keyboard) {
    display  = backlight_display_level(display);
    keyboard = backlight_keyboard_level(keyboard);
    send_user(display, keyboard, true);
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

// Runs inside esp_restart() (before the scheduler stops, newest handler first, so before esp_wifi_stop). The panel
// has no power switch and the backlight is the C6's: without this, the stopped DSI feed leaves the lit panel showing
// garbage (yellow, then gray) until the next boot resets it (#23). Backlight off first, so nothing shows while the
// panel switches off. Not run on panic, abort, watchdog or brownout resets: nothing can be done there.
static void blank_for_restart(void) {
    if (send_raw(0, 0)) {
        uart_wait_tx_done(BL_UART, pdMS_TO_TICKS(50));
    }
    display_blank();
    vTaskDelay(pdMS_TO_TICKS(BL_FRAME_MS));
}

void backlight_restore(void) {
    uint8_t display, keyboard;
    backlight_get(&display, &keyboard);
    if (send_user(display, keyboard, false)) {
        uart_wait_tx_done(BL_UART, pdMS_TO_TICKS(50));
    }
    // The C6 flasher installs the UART driver itself and fails if it is already there; backlight_start() opens it
    // again after the C6 check
    if (uart_is_driver_installed(BL_UART)) {
        uart_driver_delete(BL_UART);
    }
}

void backlight_start(void) {
    console_register("bl", "bl <display> <keyboard> (backlight 0-100, screen at least 5)", cmd_bl);
    uint8_t display, keyboard;
    backlight_get(&display, &keyboard);
    send_user(display, keyboard, false);
    // After c6link_start(): handlers run newest first, so this runs before esp_wifi_stop (which the C6 Wi-Fi setup
    // registers; there are only 5 slots in all)
    if (esp_register_shutdown_handler(blank_for_restart) != ESP_OK) {
        ESP_LOGE(TAG, "no shutdown handler slot: the screen will flash on restart");
    }
}
