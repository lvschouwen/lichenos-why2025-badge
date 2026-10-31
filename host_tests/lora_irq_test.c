// Host tests for how the C6 LoRa service reads the SX1262 IRQ flags
#include "lora_irq.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    CHECK(lora_irq_classify(0) == LORA_IRQ_NONE);
    CHECK(lora_irq_classify(LORA_IRQ_FLAG_RX_DONE) == LORA_IRQ_PACKET);
    CHECK(lora_irq_classify(LORA_IRQ_FLAG_HEADER_ERR) == LORA_IRQ_HEADER_ERROR);
    // Noise caused a header error, then a real packet completed before the task ran: keep the packet
    CHECK(lora_irq_classify(LORA_IRQ_FLAG_HEADER_ERR | LORA_IRQ_FLAG_RX_DONE) == LORA_IRQ_PACKET);
    // Preamble/header-valid flags alone are not an event
    CHECK(lora_irq_classify(0x0004 | 0x0010) == LORA_IRQ_NONE);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("lora_irq_test: all passed\n");
    return 0;
}
