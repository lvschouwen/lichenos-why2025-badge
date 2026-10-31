#include "lora_irq.h"

lora_irq_event_t lora_irq_classify(uint32_t flags) {
    // A finished packet wins: a header error from earlier noise may still be flagged next to it
    if (flags & LORA_IRQ_FLAG_RX_DONE) {
        return LORA_IRQ_PACKET;
    }
    return flags & LORA_IRQ_FLAG_HEADER_ERR ? LORA_IRQ_HEADER_ERROR : LORA_IRQ_NONE;
}
