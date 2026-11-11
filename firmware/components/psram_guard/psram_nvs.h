#pragma once
// The PSRAM guard's own NVS writes (#325): the speed record (psram_speed.c, #329), the one-shot delayline and the retry
// state (psram_boot.c). The guard runs before the settings service, so it writes NVS itself; every set, erase and commit
// is checked and a failure goes through fw_console's nvs_fail (logged once, counted in settings.write_fail /
// settings.last_err once lichen_os binds it). No heap, no PSRAM: safe at the guard's early-boot point. ESP-free apart
// from nvs.h; host-tested in host_tests/psram_nvs_test.c.
#include "nvs.h"

#include <stddef.h>

// Opens the guard's namespace read-write; a failure is reported under key (the record the caller meant to write)
esp_err_t psram_nvs_open(char const *key, nvs_handle_t *out);
// nvs_set_blob, then nvs_commit when the set worked. The first error, already reported; ESP_OK when both worked.
esp_err_t psram_nvs_put(nvs_handle_t nvs, char const *key, void const *blob, size_t len);
// nvs_erase_key, then nvs_commit. A key that is not there is the wanted end state (ESP_OK, still committed).
esp_err_t psram_nvs_drop(nvs_handle_t nvs, char const *key);
