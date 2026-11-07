#include "nvs_boot_policy.h"

nvs_boot_step_t nvs_boot_next(nvs_boot_path_t path, int err, bool retried) {
    if (err == 0) {
        return NVS_BOOT_UP;
    }
    bool layout = err == NVS_BOOT_ERR_NO_FREE_PAGES || err == NVS_BOOT_ERR_NEW_VERSION;
    // One erase at most, and never with a key burned (see the header)
    if (path == NVS_BOOT_PATH_PLAIN && layout && !retried) {
        return NVS_BOOT_ERASE_RETRY;
    }
    return NVS_BOOT_DOWN;
}

nvs_boot_init_t nvs_boot_init(bool encrypt_build, bool key_burned) {
    if (!key_burned) {
        return NVS_BOOT_INIT_PLAIN;
    }
    return encrypt_build ? NVS_BOOT_INIT_KEYED : NVS_BOOT_INIT_NONE;
}

nvs_boot_path_t nvs_boot_path_of(nvs_boot_init_t init) {
    return init == NVS_BOOT_INIT_PLAIN ? NVS_BOOT_PATH_PLAIN : NVS_BOOT_PATH_KEYED;
}
