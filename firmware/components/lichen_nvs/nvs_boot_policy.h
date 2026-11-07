#pragma once

#include <stdbool.h>

// What the boot does with each lichen_nvs_init() result (#116), pure and host-tested (host_tests/nvs_boot_policy_test.c).
// main.c asks after every init; the answer depends on whether this init went the burned-key path.
//
// Plain path (no key burned, or a build without LICHEN_NVS_ENCRYPT): an incompatible layout (no free pages, a newer
// NVS version) is erased once and init again, so the badge keeps working settings rather than none. Burned-key path:
// never erased from here, whatever the error. A secure init that fails may mean a key or partition this boot could
// not use, and an erase would turn that into lost settings, the OTA token copy and the Wi-Fi password for good. The
// badge stays up with settings unavailable; a later boot, a restore over USB or a factory reset decides.

// IDF's ESP_ERR_NVS_NO_FREE_PAGES and ESP_ERR_NVS_NEW_VERSION_FOUND (nvs.h), copied so this file stays ESP-free;
// lichen_nvs.c checks them against IDF at compile time
#define NVS_BOOT_ERR_NO_FREE_PAGES 0x110d
#define NVS_BOOT_ERR_NEW_VERSION   0x1110

typedef enum {
    NVS_BOOT_PATH_PLAIN, // nvs_flash_init(): no key in the eFuse block, or the feature not built in
    NVS_BOOT_PATH_KEYED, // an HMAC key is burned: secure init, migration or refusal
} nvs_boot_path_t;

typedef enum {
    NVS_BOOT_UP,          // NVS works
    NVS_BOOT_ERASE_RETRY, // erase the partition and init once more
    NVS_BOOT_DOWN,        // report it and boot on without settings; nothing is erased
} nvs_boot_step_t;

// err is the esp_err_t of the init (0 = ESP_OK); retried says this init already followed the erase
nvs_boot_step_t nvs_boot_next(nvs_boot_path_t path, int err, bool retried);
