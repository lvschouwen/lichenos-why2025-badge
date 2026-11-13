#pragma once

#include "boot_keys.h"

// Presses k's keys now and releases them after its hold (#379). Call once, after factory_combo_start has added its
// raw hook, so the combo sees the presses the way it sees keys held at boot. Returns at once.
void boot_keys_play(boot_keys_t k);
