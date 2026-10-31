#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What a DIO1 wake-up of the C6 LoRa service means, from the SX1262 IRQ status (#51). Pure logic, no ESP headers:
// built into the host tests as well as the C6 image.

// SX1262 IRQ status bits (datasheet table 13-29; RadioLib RADIOLIB_SX126X_IRQ_*)
#define LORA_IRQ_FLAG_RX_DONE    0x0002u
#define LORA_IRQ_FLAG_HEADER_ERR 0x0020u

typedef enum {
    LORA_IRQ_NONE,
    LORA_IRQ_PACKET,       // read it; readData() reports a CRC error itself
    LORA_IRQ_HEADER_ERROR, // a header error and no packet
} lora_irq_event_t;

lora_irq_event_t lora_irq_classify(uint32_t flags);

#ifdef __cplusplus
}
#endif
