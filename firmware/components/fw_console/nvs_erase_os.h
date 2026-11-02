#pragma once
// OS only, never an app (#84 T9, #123): the real nvs_flash_erase_partition, past the refusing wrap in console.c. Its
// two callers are main.c (NVS unusable at boot) and factory_reset.c (#66); every other caller lands in the wrap.

#include "esp_err.h"

esp_err_t __real_nvs_flash_erase_partition(char const *part_name);
