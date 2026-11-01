#pragma once
// The P4's one SDMMC controller is shared (#60): the C6 link (esp_hosted) on slot 1, the SD card on slot 0. IDF's
// sdmmc_host_init / sdmmc_host_init_slot / sdmmc_host_deinit_slot read and write the controller handle without a
// lock, so whoever creates, adds to or tears down a slot holds this lock around exactly that: sdcard.c around its
// host init and slot deinit, c6link around esp_hosted_init and esp_hosted_deinit (they run the slot-1 bring-up and
// teardown). Never held across a wait for the other chip (esp_hosted_connect_to_slave) or file I/O. Recursive.

#include <stdbool.h>
#include <stdint.h>

// False when the lock was not free within wait_ms (UINT32_MAX: wait for good)
bool board_sdmmc_lock(uint32_t wait_ms);
void board_sdmmc_unlock(void);
