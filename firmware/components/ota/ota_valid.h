#pragma once

#include <stdbool.h>
#include <stdint.h>

// Pure logic, no ESP headers: also built into the host tests.

// How long a new image must run with everything up before it counts as good: a build that crashes or hangs within
// this time still rolls back on the next reset
#define OTA_VALID_AFTER_S 45

// A new image runs on trial (pending_verify) until it proves itself: the UI and the C6 link are up and it has run for
// OTA_VALID_AFTER_S. Wi-Fi is not required, so a badge without (working) Wi-Fi credentials doesn't roll back for ever
// (#61).
bool ota_should_mark_valid(bool pending_verify, bool ui_up, bool c6_up, uint32_t uptime_s);

// How long past OTA_VALID_AFTER_S a trial image may stay without its C6 link before it gives up
#define OTA_ROLLBACK_GRACE_S 60

// A trial image whose C6 link never came up has no Wi-Fi, so nobody can fix it over the air: restart so the bootloader
// boots the previous image (#36). With the C6 up (UI down only) Wi-Fi works and the image stays on trial.
bool ota_should_roll_back_now(bool pending_verify, bool c6_up, uint32_t uptime_s);
