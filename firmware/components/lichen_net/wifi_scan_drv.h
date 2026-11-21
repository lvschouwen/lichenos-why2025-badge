#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wifi_service.h"

#include <stdbool.h>
#include <stddef.h>

// The blocking driver scan, split out of wifi_service.c (#502, file size)
#define WIFI_SCAN_RECORDS 32
#define WIFI_SCAN_WAIT_MS 6000

// Scans, waits for done (given by the SCAN_DONE event) at most WIFI_SCAN_WAIT_MS, and copies up to max records into out
// (wifi_list_clean first when clean_list). The c6wifi task only. False when nothing was read.
bool wifi_scan_drv(SemaphoreHandle_t done, wifi_ap_t *out, size_t max, size_t *n, bool clean_list);
