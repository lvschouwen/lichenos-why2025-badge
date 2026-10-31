#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What the badge says after the reboot into an update (#76). Pure, host-tested in host_tests/update_status_test.c.
typedef enum { UPD_NONE, UPD_UPDATED, UPD_ROLLED_BACK } upd_result_t;

// prev_version is what ran before the update (NVS upd_from, NULL or empty when nothing is stored); rolled_back: the
// bootloader went back to the old image because the new one failed.
upd_result_t update_result(char const *prev_version, char const *running_version, bool rolled_back);
// "Updated to <run>" / "Update failed, back on <run>", cut to cap; "" for UPD_NONE. Returns the length written.
size_t update_line(upd_result_t r, char const *prev, char const *run, char *out, size_t cap);
// done of total as 0..100, or -1 while the total is unknown (0)
int update_pct(uint32_t done, uint32_t total);

// How long a transfer may go without news before the screen gives up on the sender. Once a firmware image is fully
// received the flash write and the checks report nothing and can take a while, so the limit is much longer.
uint32_t update_stall_limit_ms(bool firmware, uint32_t done, uint32_t total);

// What to do at boot with a stored upd_from. The line is shown at once; the stored version is forgotten only when the
// running image is valid, so a crash during the trial (bootloader rolls back) still leaves the old image something to say.
typedef struct {
    upd_result_t result;
    bool         erase;
} upd_boot_t;
upd_boot_t update_after_boot(char const *prev_version, char const *running_version, bool rolled_back, bool image_valid);
