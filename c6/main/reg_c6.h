// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
// The C6's settings registry (#307/#309): the keys the C6 serves to the P4 over REG_GET / REG_SET. Served on the LoRa
// task only (lora_service.cpp), so the LoRa getters need no lock; the backlight takes its own.
#pragma once

#include "reg.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern reg_entry_t const reg_c6_table[];
extern size_t const      reg_c6_count;

// esp_bt_controller_get_status(), as an esp_bt_controller_status_t (app_main.c; a fake in the host test)
uint8_t c6_bt_status(void);

#ifdef __cplusplus
}
#endif
