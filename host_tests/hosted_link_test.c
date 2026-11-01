// Host tests for the ESP-Hosted transport-failure latch that makes RPCs fail at once against a dead C6 (#90)
#include "eh_host_mcu_transport_state.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void up(void) {
    eh_host_mcu_transport_state_set(EH_HOST_MCU_TRANSPORT_RX_ACTIVE);
    eh_host_mcu_transport_state_set(EH_HOST_MCU_TRANSPORT_TX_ACTIVE);
}

static void starts_not_failed(void) {
    CHECK(!eh_host_mcu_transport_state_is_failed());
    CHECK(!eh_host_mcu_transport_state_is_tx_ready());
}

// The failure latches without touching the TX state the bus driver gates on
static void failure_latches(void) {
    up();
    CHECK(!eh_host_mcu_transport_state_is_failed());
    eh_host_mcu_transport_state_set_failed();
    CHECK(eh_host_mcu_transport_state_is_failed());
    CHECK(eh_host_mcu_transport_state_is_tx_ready());
    eh_host_mcu_transport_state_set_failed(); // a flood of failure posts
    CHECK(eh_host_mcu_transport_state_is_failed());
}

// The re-attach: deinit (INACTIVE), init (RX_ACTIVE), the C6's INIT event (TX_ACTIVE) each clear it
static void reattach_clears(void) {
    eh_host_mcu_transport_state_t const steps[] = {EH_HOST_MCU_TRANSPORT_INACTIVE, EH_HOST_MCU_TRANSPORT_RX_ACTIVE,
                                                   EH_HOST_MCU_TRANSPORT_TX_ACTIVE};
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        up();
        eh_host_mcu_transport_state_set_failed();
        eh_host_mcu_transport_state_set(steps[i]);
        CHECK(!eh_host_mcu_transport_state_is_failed());
    }
}

// A frame from the slave means the failure was transient: RPCs may wait for replies again
static void rx_clears(void) {
    up();
    eh_host_mcu_transport_state_set_failed();
    eh_host_mcu_transport_state_note_rx();
    CHECK(!eh_host_mcu_transport_state_is_failed());
    eh_host_mcu_transport_state_note_rx(); // and is harmless when nothing failed
    CHECK(!eh_host_mcu_transport_state_is_failed());
    CHECK(eh_host_mcu_transport_state_is_tx_ready());
}

int main(void) {
    starts_not_failed();
    failure_latches();
    reattach_clears();
    rx_clears();
    if (failures) {
        printf("hosted_link_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("hosted_link_test: all passed\n");
    return 0;
}
