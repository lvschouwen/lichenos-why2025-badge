#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sensors_chips.h"

#define XFER_TIMEOUT_MS 20 // a 32-byte write at 100 kHz takes about 3 ms
#define MAX_WRITE       64 // the Bosch APIs write at most read_write_len (32) bytes at once

i2c_master_dev_handle_t sensors_bus_dev(i2c_master_bus_handle_t bus, uint8_t addr, i2c_master_dev_handle_t *slot) {
    if (*slot) {
        return *slot;
    }
    if (!bus || i2c_master_probe(bus, addr, XFER_TIMEOUT_MS) != ESP_OK) {
        return NULL;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = CONFIG_LICHEN_SENSORS_I2C_HZ,
    };
    if (i2c_master_bus_add_device(bus, &cfg, slot) != ESP_OK) {
        *slot = NULL;
    }
    return *slot;
}

int8_t sensors_bus_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, uint32_t len) {
    return dev && i2c_master_transmit_receive(dev, &reg, 1, data, len, XFER_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

int8_t sensors_bus_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t const *data, uint32_t len) {
    if (!dev || len > MAX_WRITE) {
        return -1;
    }
    // One transaction, register then data, without copying the data into a scratch buffer
    i2c_master_transmit_multi_buffer_info_t parts[2] = {
        {.write_buffer = &reg, .buffer_size = 1},
        {.write_buffer = (uint8_t *)data, .buffer_size = len},
    };
    return i2c_master_multi_buffer_transmit(dev, parts, len ? 2 : 1, XFER_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

// The Bosch APIs wait between 2 us (between config writes) and 10 ms (soft reset, polls): busy-wait the short ones,
// sleep the rest so the task gives the CPU (and the bus) away
void sensors_delay_us(uint32_t us) {
    if (us < 1000) {
        esp_rom_delay_us(us);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000) + 1);
}
