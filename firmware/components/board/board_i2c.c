#include "board_i2c.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "fw_console.h"

#include <string.h>

static char const TAG[] = "board";

static i2c_master_bus_handle_t bus;
static i2c_master_bus_handle_t bus2; // the camera header's (#11), NULL while closed

i2c_master_bus_handle_t board_i2c_bus(void) {
    if (bus) {
        return bus;
    }
    i2c_master_bus_config_t cfg = {
        .i2c_port                     = I2C_NUM_0,
        .sda_io_num                   = BOARD_I2C_SDA,
        .scl_io_num                   = BOARD_I2C_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true, // as the keyboard driver had it
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c0 (sda %d, scl %d): %s", BOARD_I2C_SDA, BOARD_I2C_SCL, esp_err_to_name(err));
        bus = NULL;
    }
    return bus;
}

// Both lines must idle high through R7/R8. Read as plain inputs, no pulls of our own (an internal pull-up would make an
// unwired header look idle), output driver off: the same state the pins are left in after a close.
static bool i2c2_lines_idle(void) {
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << BOARD_I2C2_SDA) | (1ULL << BOARD_I2C2_SCL),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&in) != ESP_OK) {
        return false;
    }
    return gpio_get_level(BOARD_I2C2_SDA) == 1 && gpio_get_level(BOARD_I2C2_SCL) == 1;
}

esp_err_t board_i2c2_open(i2c_master_bus_handle_t *out) {
    *out = NULL;
    if (bus2) {
        *out = bus2;
        return ESP_OK;
    }
    if (!i2c2_lines_idle()) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_master_bus_config_t cfg = {
        .i2c_port                     = BOARD_I2C2_PORT,
        .sda_io_num                   = BOARD_I2C2_SDA,
        .scl_io_num                   = BOARD_I2C2_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = false, // R7/R8 are fitted; see i2c2_lines_idle
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &bus2);
    if (err != ESP_OK) {
        bus2 = NULL;
        return err;
    }
    *out = bus2;
    return ESP_OK;
}

void board_i2c2_close(void) {
    if (!bus2) {
        return;
    }
    // The driver's release turns the output drivers off and detaches the controller's signals from the pins
    esp_err_t err = i2c_del_master_bus(bus2);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c1 close: %s", esp_err_to_name(err)); // a device still attached: the bus stays open
        return;
    }
    bus2 = NULL;
}

// Probes every 7-bit address a device may use (0x08-0x77); the keyboard chip (0x34) is the known one.
static void cmd_i2c(char const *args) {
    if (strcmp(args, "scan") != 0) {
        console_reply("i2c scan");
        return;
    }
    i2c_master_bus_handle_t b = board_i2c_bus();
    if (!b) {
        console_reply("i2c: no bus");
        return;
    }
    char line[96];
    int  n = 0, len = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(b, addr, 20) != ESP_OK) {
            continue;
        }
        n++;
        if (len < (int)sizeof line - 6) {
            len += snprintf(line + len, sizeof line - (size_t)len, " 0x%02x", addr);
        }
    }
    console_reply("i2c: %d device(s)%s", n, n ? line : "");
}

void board_i2c_register_console(void) {
    console_register("i2c", "i2c scan (addresses that answer on the badge bus)", cmd_i2c);
}
