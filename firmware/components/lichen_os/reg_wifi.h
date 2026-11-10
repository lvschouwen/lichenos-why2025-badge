#pragma once

#include "reg_cmd.h"
#include "wifi_regdom.h"

#include <stdbool.h>
#include <stdint.h>

// The wifi.* keys of the P4 table (#311) from the Wi-Fi service's read-back (wifi_readback, lichen_net). Pure,
// host-tested (wifi_reg_test.c); lichen_reg.c reads and passes it in.
//
// One view per key: applied from the driver (range-checked against the key, so a power save the table does not name
// is "failed", not a wrong name), requested from what the P4 asked for (the country always NL; 0 = the driver's own
// choice, no requested), the age of the read, and the note: no read since boot = "no link to the C6", that part of the
// read failed = "failed", a live ask that got nothing = "stale" (the cache before is shown).
bool reg_wifi_is_key(reg_entry_t const *e);
void reg_wifi_view(reg_entry_t const *e, wifi_readback_t const *r, bool fresh, bool live, uint32_t now_ms,
                   reg_view_t *v);
