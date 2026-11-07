// nvs_boot_policy.c (#116): what boot does with each lichen_nvs_init() result. The burned-key path never erases.
#include "nvs_boot_policy.h"

#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// Error codes lichen_nvs_init() can return besides the two layout ones (IDF values: nvs.h, esp_err.h)
static int const other_errs[] = {
    -1,     // ESP_FAIL: derivation failed on an encrypted partition
    0x103,  // ESP_ERR_INVALID_STATE: partition unreadable with a key burned (REFUSE)
    0x101,  // ESP_ERR_NO_MEM
    0x110f, // ESP_ERR_NVS_PART_NOT_FOUND
    0x1111, // ESP_ERR_NVS_XTS_ENCR_FAILED
    0x1112, // ESP_ERR_NVS_XTS_DECR_FAILED
    0x1113, // ESP_ERR_NVS_XTS_CFG_FAILED
    0x1119, // ESP_ERR_NVS_WRONG_ENCRYPTION
};
#define N_OTHER (sizeof(other_errs) / sizeof(other_errs[0]))

static void ok_is_up_on_both_paths(void) {
    CHECK(nvs_boot_next(NVS_BOOT_PATH_PLAIN, 0, false) == NVS_BOOT_UP);
    CHECK(nvs_boot_next(NVS_BOOT_PATH_PLAIN, 0, true) == NVS_BOOT_UP);
    CHECK(nvs_boot_next(NVS_BOOT_PATH_KEYED, 0, false) == NVS_BOOT_UP);
    CHECK(nvs_boot_next(NVS_BOOT_PATH_KEYED, 0, true) == NVS_BOOT_UP);
}

// Today's behaviour, kept: an incompatible plain layout is erased once
static void plain_layout_errors_erase_once(void) {
    CHECK(nvs_boot_next(NVS_BOOT_PATH_PLAIN, NVS_BOOT_ERR_NO_FREE_PAGES, false) == NVS_BOOT_ERASE_RETRY);
    CHECK(nvs_boot_next(NVS_BOOT_PATH_PLAIN, NVS_BOOT_ERR_NEW_VERSION, false) == NVS_BOOT_ERASE_RETRY);
    // The init after the erase fails again: no second erase, boot on without settings
    CHECK(nvs_boot_next(NVS_BOOT_PATH_PLAIN, NVS_BOOT_ERR_NO_FREE_PAGES, true) == NVS_BOOT_DOWN);
    CHECK(nvs_boot_next(NVS_BOOT_PATH_PLAIN, NVS_BOOT_ERR_NEW_VERSION, true) == NVS_BOOT_DOWN);
}

static void plain_other_errors_stay_down(void) {
    for (unsigned i = 0; i < N_OTHER; i++) {
        CHECK(nvs_boot_next(NVS_BOOT_PATH_PLAIN, other_errs[i], false) == NVS_BOOT_DOWN);
        CHECK(nvs_boot_next(NVS_BOOT_PATH_PLAIN, other_errs[i], true) == NVS_BOOT_DOWN);
    }
}

// The bug this file exists for: a secure_init that returns NO_FREE_PAGES/NEW_VERSION hit main.c's erase
static void keyed_never_erases(void) {
    CHECK(nvs_boot_next(NVS_BOOT_PATH_KEYED, NVS_BOOT_ERR_NO_FREE_PAGES, false) == NVS_BOOT_DOWN);
    CHECK(nvs_boot_next(NVS_BOOT_PATH_KEYED, NVS_BOOT_ERR_NEW_VERSION, false) == NVS_BOOT_DOWN);
    CHECK(nvs_boot_next(NVS_BOOT_PATH_KEYED, NVS_BOOT_ERR_NO_FREE_PAGES, true) == NVS_BOOT_DOWN);
    for (unsigned i = 0; i < N_OTHER; i++) {
        CHECK(nvs_boot_next(NVS_BOOT_PATH_KEYED, other_errs[i], false) == NVS_BOOT_DOWN);
        CHECK(nvs_boot_next(NVS_BOOT_PATH_KEYED, other_errs[i], true) == NVS_BOOT_DOWN);
    }
}

// main.c's loop: at most one erase whatever init keeps returning, and none on the keyed path
static unsigned erases_for(nvs_boot_path_t path, int err) {
    unsigned        erases = 0;
    nvs_boot_step_t step   = nvs_boot_next(path, err, false);
    if (step == NVS_BOOT_ERASE_RETRY) {
        erases++;
        step = nvs_boot_next(path, err, true);
    }
    CHECK(step != NVS_BOOT_ERASE_RETRY);
    return erases;
}

static void boot_sequence_erases_at_most_once(void) {
    CHECK(erases_for(NVS_BOOT_PATH_PLAIN, NVS_BOOT_ERR_NO_FREE_PAGES) == 1);
    CHECK(erases_for(NVS_BOOT_PATH_PLAIN, NVS_BOOT_ERR_NEW_VERSION) == 1);
    CHECK(erases_for(NVS_BOOT_PATH_KEYED, NVS_BOOT_ERR_NO_FREE_PAGES) == 0);
    CHECK(erases_for(NVS_BOOT_PATH_KEYED, NVS_BOOT_ERR_NEW_VERSION) == 0);
    for (unsigned i = 0; i < N_OTHER; i++) {
        CHECK(erases_for(NVS_BOOT_PATH_PLAIN, other_errs[i]) == 0);
        CHECK(erases_for(NVS_BOOT_PATH_KEYED, other_errs[i]) == 0);
    }
}

int main(void) {
    ok_is_up_on_both_paths();
    plain_layout_errors_erase_once();
    plain_other_errors_stay_down();
    keyed_never_erases();
    boot_sequence_erases_at_most_once();
    if (fails) {
        printf("%d failure(s)\n", fails);
        return 1;
    }
    printf("All nvs_boot_policy tests passed\n");
    return 0;
}
