// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// The SX1262 receiver for the P4 (#51): listens on peer data for commands, owns the radio in one task and pushes
// every received packet to the P4. Receive only. Returns false (and logs why) if it could not start; Wi-Fi and BLE
// keep running either way.
bool lora_service_start(void);

#ifdef __cplusplus
}
#endif
