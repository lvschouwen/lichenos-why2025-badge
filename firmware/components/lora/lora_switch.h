#pragma once

#include <stdbool.h>

// lora's own side of the radio switch (#222), not for other components: the console's "fw lora on|off" asks here.
// The owner (lora_set_switch_owner, lora_os.h) saves the switch and tells everyone, lora_set_enabled included; without
// an owner the request switches LoRa alone (lora_set_enabled, not saved, the OS switch state is not told). Returns
// lora_enabled() afterwards: what lora_send goes by. Pure and host-tested (host_tests/lora_switch_test.c).
bool lora_switch_request(bool on);
