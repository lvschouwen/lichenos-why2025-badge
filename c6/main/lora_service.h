// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lora_proto.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The SX1262 receiver for the P4 (#51): listens on peer data for commands, owns the radio in one task and pushes
// every received packet to the P4. Receive only. Returns false (and logs why) if it could not start; Wi-Fi and BLE
// keep running either way.
bool lora_service_start(void);

// What the SX1262 runs now (#309), in lora_config_t's units: frequency, bandwidth, SF, CR and preamble from RadioLib's
// driver state, the sync word from the chip's registers, the power as the dBm of the last start the radio accepted
// (RadioLib keeps only the PA register value, which several dBm map to). false while the radio has not been started
// since boot, after a start that failed, or when a value does not decode. LoRa task only (the registry runs there).
bool lora_service_readback(lora_config_t *out);

// The service's lora_state_t. LoRa task only.
uint8_t lora_service_state(void);

#ifdef __cplusplus
}
#endif
