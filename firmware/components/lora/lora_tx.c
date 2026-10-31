#include "lora_tx.h"

bool lora_tx_begin(lora_tx_t *tx, uint32_t now_ms, uint32_t airtime_ms) {
    if (tx->state == LORA_TX_BUSY) {
        return false;
    }
    if (tx->unreachable) {
        tx->state = LORA_TX_FAILED;
        return true;
    }
    tx->state       = LORA_TX_BUSY;
    tx->deadline_ms = now_ms + airtime_ms + LORA_TX_GRACE_MS;
    return true;
}

void lora_tx_link(lora_tx_t *tx, bool reachable) {
    tx->unreachable = !reachable;
}

void lora_tx_result(lora_tx_t *tx, uint8_t code) {
    if (tx->state == LORA_TX_BUSY) {
        tx->state = code == LORA_RESULT_OK ? LORA_TX_DONE : LORA_TX_FAILED;
    }
}

lora_tx_state_t lora_tx_poll(lora_tx_t *tx, uint32_t now_ms) {
    if (tx->state == LORA_TX_BUSY && (int32_t)(now_ms - tx->deadline_ms) >= 0) {
        tx->state = LORA_TX_FAILED;
    }
    return tx->state;
}

void lora_tx_finish(lora_tx_t *tx) {
    if (tx->state != LORA_TX_BUSY) {
        tx->state = LORA_TX_IDLE;
    }
}
