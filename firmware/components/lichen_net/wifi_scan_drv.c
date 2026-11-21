// The blocking driver scan (wifi_scan_drv.h)
#include "wifi_scan_drv.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "wifi_list.h"

#include <stdlib.h>
#include <string.h>

static char const TAG[] = "c6link";

// On the c6wifi task only (#502): the driver is that task's alone. done: the binary semaphore the SCAN_DONE event gives.
bool wifi_scan_drv(SemaphoreHandle_t done, wifi_ap_t *out, size_t max, size_t *n, bool clean_list) {
    *n = 0;
    if (!out || max == 0) {
        return false;
    }
    bool                 ok = false;
    wifi_scan_config_t   sc = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE};
    sc.scan_time.active.min = 100;
    sc.scan_time.active.max = 200;
    xSemaphoreTake(done, 0);
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start: %s", esp_err_to_name(err));
    } else if (xSemaphoreTake(done, pdMS_TO_TICKS(WIFI_SCAN_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "scan did not finish in %d ms", WIFI_SCAN_WAIT_MS);
        esp_wifi_scan_stop();
    } else {
        uint16_t         count = WIFI_SCAN_RECORDS;
        wifi_ap_record_t *recs = calloc(WIFI_SCAN_RECORDS, sizeof(*recs));
        wifi_ap_t        *aps  = calloc(WIFI_SCAN_RECORDS, sizeof(*aps));
        if (recs && aps) {
            // Fetching also frees the driver's list, even if we use none of it
            if (esp_wifi_scan_get_ap_records(&count, recs) == ESP_OK) {
                for (uint16_t i = 0; i < count; i++) {
                    size_t len = strnlen((char const *)recs[i].ssid, 32);
                    memcpy(aps[i].ssid, recs[i].ssid, len);
                    aps[i].ssid[len] = '\0';
                    aps[i].rssi      = recs[i].rssi;
                    aps[i].open      = recs[i].authmode == WIFI_AUTH_OPEN;
                    aps[i].channel   = recs[i].primary;
                }
                size_t clean = clean_list ? wifi_list_clean(aps, count) : count;
                *n           = clean < max ? clean : max;
                memcpy(out, aps, *n * sizeof(*out));
                ok = true;
            }
        } else {
            esp_wifi_clear_ap_list();
        }
        free(recs);
        free(aps);
    }
    return ok;
}
