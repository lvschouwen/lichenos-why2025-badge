// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lora_proto.h"

// lora_decode before #212, kept as the oracle for the differential test
lora_dec_t lora_decode_ref(uint8_t const *buf, size_t len, lora_msg_t *out);
