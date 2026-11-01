#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "update_status.h"

// "Updating Lichen" (#76). begin/progress/end are called from the OTA task and only store numbers; an LVGL timer opens
// and refreshes the screen. total 0 = unknown, name NULL for firmware; done counts the same bytes as total.
// image: firmware or assets, which stop everything and end in a restart; firmware: a new version ("Updated" next boot)
void update_screen_begin(bool image, bool firmware, char const *name, uint32_t total);
void update_screen_progress(uint32_t done);
void update_screen_end(bool ok, bool rebooting);
// After boot: says "Updated to ..." or "Update failed, back on ..." on home for 4 s when the badge has just been through
// an update, then forgets it. Call once the shell is started.
void update_screen_boot_check(void);
// What update_screen_boot_check found (UPD_NONE before it ran or when there was no update) and its line ("Updated to
// fw-..."), for the "Lichen updated" notification (#62); any task
upd_result_t update_screen_boot_result(char *line, size_t cap);
// The running image has been marked valid: forget the stored previous version (kept until now, so a crash during the
// trial still lets the image the bootloader falls back to say "Update failed").
void update_screen_image_valid(void);
// Esc must not close the screen while a firmware update runs (the "Don't turn off the badge" notice stays)
bool update_screen_blocks_back(void);
// A transfer is under way, or update mode has everything stopped (the screen timeout keeps the screen lit, #43)
bool update_screen_busy(void);
// POST /prepare (#76): enters update mode now (apps stopped, screens closed, update screen up) and returns when done;
// may block for seconds, OTA task only. A firmware begin must follow within UPDATE_PREPARE_TIMEOUT_MS or the badge restarts.
void update_screen_prepare(void);
// false while a firmware update has everything stopped: file pushes are refused
bool update_screen_file_ok(void);
