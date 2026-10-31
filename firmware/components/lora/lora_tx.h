#pragma once

#include "lora_proto.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One packet sent through the C6 at a time (#65): it is done when the C6's RESULT says so, failed when the RESULT
// says otherwise or none comes within its airtime + 2 s (C6 down or reflashing). Pure logic, no ESP headers.

#define LORA_TX_GRACE_MS 2000

typedef enum {
    LORA_TX_IDLE,
    LORA_TX_BUSY,
    LORA_TX_DONE,
    LORA_TX_FAILED,
} lora_tx_state_t;

typedef struct {
    lora_tx_state_t state;
    uint32_t        deadline_ms;
    bool            unreachable; // the last peer-data send to the C6 failed and the C6 hasn't spoken since
} lora_tx_t;

// Whether the C6 can be reached: while it can't, a send fails at once (each peer-data send would block ~5 s)
void            lora_tx_link(lora_tx_t *tx, bool reachable);
// false while a packet is still on its way
bool            lora_tx_begin(lora_tx_t *tx, uint32_t now_ms, uint32_t airtime_ms);
void            lora_tx_result(lora_tx_t *tx, uint8_t code); // lora_result_code_t of the C6's RESULT for the SEND
lora_tx_state_t lora_tx_poll(lora_tx_t *tx, uint32_t now_ms);
void            lora_tx_finish(lora_tx_t *tx); // after DONE or FAILED has been seen: back to IDLE

#ifdef __cplusplus
}
#endif
