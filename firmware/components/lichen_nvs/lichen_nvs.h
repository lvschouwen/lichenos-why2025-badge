#pragma once

#include "esp_err.h"
#include "nvs_boot_policy.h"

// NVS init for the OS (#116). Without CONFIG_LICHEN_NVS_ENCRYPT this is exactly nvs_flash_init(). With it, NVS opens
// encrypted when an HMAC key is already burned in the configured eFuse block (a plaintext partition is migrated first)
// and plain otherwise. Never burns an eFuse. Every caller of nvs_flash_init() in the OS goes through here, because an
// init on an encrypted partition with the plain call finds noise and erases it entry by entry.
// Also registers "fw nvs-slots" (every build) and "fw nvs-enc" (encrypt builds).
esp_err_t lichen_nvs_init(void);

// The path the last lichen_nvs_init() took: KEYED when it found an HMAC key burned. main.c passes it with the result
// to nvs_boot_next(), which never erases on the KEYED path.
nvs_boot_path_t lichen_nvs_boot_path(void);

// True when this boot migrated the partition to encrypted NVS (#116): no other settings migration may share the boot
// (#288's move of the apps' secrets waits for the next one)
bool lichen_nvs_migrated(void);

// main.c tells the module how the boot ended up (for "fw nvs-enc"): NVS up, or down with nothing erased
void lichen_nvs_boot_done(bool up);
