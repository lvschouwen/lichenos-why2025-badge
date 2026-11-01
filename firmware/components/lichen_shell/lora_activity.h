#pragma once

#include <stdbool.h>
#include <stdint.h>

// The status bar's LoRa TX/RX chevrons (#29), pure: a packet counter that moved lights a chevron in full colour for
// LORA_ACT_HOLD_MS, then it fades linearly to faint over LORA_ACT_FADE_MS. One lora_act_t per direction.

#define LORA_ACT_HOLD_MS 500
#define LORA_ACT_FADE_MS 1000

typedef struct {
    uint32_t seen;      // the count at the last step
    uint32_t lit_at_ms; // when it last moved
    bool     primed;    // the first step only takes the count: what is already there at boot does not flash
    bool     lit;
} lora_act_t;

// 255 = full colour .. 0 = faint. The counters only go up, so any change (a wrap included) is new traffic, and a burst
// between two steps shows as one blink. Clock and count compare wrap-safe.
uint8_t lora_act_step(lora_act_t *a, uint32_t count, uint32_t now_ms);
