#pragma once
// OS only, never an app (#84 T9, #123): the real nvs_flash_erase_partition, past the refusing wrap in console.c. Its
// callers are main.c (NVS unusable at boot), factory_reset.c (#66) and lichen_nvs.c (the one-time migration to encrypted
// NVS, #116, only with LICHEN_NVS_ENCRYPT); every other caller lands in the wrap.

#include "esp_err.h"

esp_err_t __real_nvs_flash_erase_partition(char const *part_name);
