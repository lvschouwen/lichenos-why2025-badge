#pragma once

#include "esp_err.h"

// NVS init for the OS (#116). Without CONFIG_LICHEN_NVS_ENCRYPT this is exactly nvs_flash_init(). With it, NVS opens
// encrypted when an HMAC key is already burned in the configured eFuse block (a plaintext partition is migrated first)
// and plain otherwise. Never burns an eFuse. Every caller of nvs_flash_init() in the OS goes through here, because an
// init on an encrypted partition with the plain call finds noise and (main.c) erases it.
esp_err_t lichen_nvs_init(void);
