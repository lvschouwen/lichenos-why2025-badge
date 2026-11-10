#pragma once

#include "esp_partition.h"

// "fw slot" shows each app slot's OTA state. ota owns that state (#285) and sits above fw_console, so main binds its
// reader (ota_slot_state_name) here, in start_console. The OS's header: not for apps.
typedef char const *(*fw_slot_state_fn)(esp_partition_t const *p);
// Unbound, or bound to NULL: every slot shows as "unknown"
void console_bind_slot_state(fw_slot_state_fn state);
