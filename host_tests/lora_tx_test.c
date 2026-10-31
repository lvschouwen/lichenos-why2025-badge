// Host test: the P4's bookkeeping for one LoRa packet sent through the C6 (#65): done, failed, or given up on
#include "lora_tx.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    lora_tx_t tx = {0};
    CHECK(lora_tx_poll(&tx, 0) == LORA_TX_IDLE);

    // Sent, and the C6 says it left the radio
    CHECK(lora_tx_begin(&tx, 1000, 300));
    CHECK(!lora_tx_begin(&tx, 1001, 300)); // one at a time
    CHECK(lora_tx_poll(&tx, 1100) == LORA_TX_BUSY);
    lora_tx_result(&tx, LORA_RESULT_OK);
    CHECK(lora_tx_poll(&tx, 1200) == LORA_TX_DONE);
    lora_tx_finish(&tx);
    CHECK(lora_tx_poll(&tx, 1200) == LORA_TX_IDLE);

    // The C6 refuses or the radio fails
    CHECK(lora_tx_begin(&tx, 2000, 300));
    lora_tx_result(&tx, LORA_RESULT_RADIO_ERROR);
    CHECK(lora_tx_poll(&tx, 2001) == LORA_TX_FAILED);
    lora_tx_finish(&tx);

    // No answer (C6 down or reflashing): given up airtime + 2 s after sending, a late answer changes nothing
    CHECK(lora_tx_begin(&tx, 5000, 300));
    CHECK(lora_tx_poll(&tx, 5000 + 300 + 1999) == LORA_TX_BUSY);
    CHECK(lora_tx_poll(&tx, 5000 + 300 + 2000) == LORA_TX_FAILED);
    lora_tx_result(&tx, LORA_RESULT_OK);
    CHECK(lora_tx_poll(&tx, 9000) == LORA_TX_FAILED);
    lora_tx_finish(&tx);

    // An answer nobody waits for is ignored
    lora_tx_result(&tx, LORA_RESULT_OK);
    CHECK(lora_tx_poll(&tx, 9000) == LORA_TX_IDLE);

    // Across the millisecond counter's wrap
    CHECK(lora_tx_begin(&tx, 0xFFFFFF00u, 300));
    CHECK(lora_tx_poll(&tx, 0x00000100u) == LORA_TX_BUSY);
    CHECK(lora_tx_poll(&tx, 0xFFFFFF00u + 2300u) == LORA_TX_FAILED);

    // C6 unreachable (a peer-data send failed and nothing has come back since): fails at once, no RPC wait
    lora_tx_t down = {0};
    lora_tx_link(&down, false);
    CHECK(lora_tx_begin(&down, 100, 300));
    CHECK(lora_tx_poll(&down, 100) == LORA_TX_FAILED);
    lora_tx_finish(&down);
    lora_tx_link(&down, true); // the C6 answered again
    CHECK(lora_tx_begin(&down, 200, 300));
    CHECK(lora_tx_poll(&down, 200) == LORA_TX_BUSY);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("lora_tx_test: all checks passed\n");
    return 0;
}
