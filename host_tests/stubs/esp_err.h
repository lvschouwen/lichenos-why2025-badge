// Host stand-in for the error codes of ESP-IDF's esp_err.h
#pragma once

typedef int esp_err_t;

#define ESP_OK              0
#define ESP_FAIL            -1
#define ESP_ERR_NO_MEM      0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_TIMEOUT     0x107
