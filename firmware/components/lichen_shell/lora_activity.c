// The status bar's LoRa TX/RX chevrons (#29): hold, then a linear fade. Host-tested in lora_activity_test.c.
#include "lora_activity.h"

uint8_t lora_act_step(lora_act_t *a, uint32_t count, uint32_t now_ms) {
    if (!a->primed) {
        a->primed = true;
        a->seen   = count;
        return 0;
    }
    if (count != a->seen) {
        a->seen      = count;
        a->lit       = true;
        a->lit_at_ms = now_ms;
    }
    if (!a->lit) {
        return 0;
    }
    uint32_t age = now_ms - a->lit_at_ms;
    if (age <= LORA_ACT_HOLD_MS) {
        return 255;
    }
    if (age >= LORA_ACT_HOLD_MS + LORA_ACT_FADE_MS) {
        a->lit = false;
        return 0;
    }
    return (uint8_t)(255 - (age - LORA_ACT_HOLD_MS) * 255 / LORA_ACT_FADE_MS);
}
