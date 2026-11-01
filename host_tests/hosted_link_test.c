// Host tests for the ESP-Hosted transport-failure latch that makes RPCs fail at once against a dead C6 (#90)
#include "eh_host_feat_rpc_slice.h"
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

#define POLL 50u

// A sync RPC wait is cut into slices so a failure in between ends it: the slices of one wait, as the loop sees them
static unsigned slices(uint32_t timeout, uint32_t *out, bool *more_out, unsigned max) {
    uint32_t left = timeout;
    unsigned n    = 0;
    bool     more = true;
    while (more && n < max) {
        out[n]      = eh_rpc_next_slice(&left, POLL, &more);
        more_out[n] = more;
        n++;
    }
    return n;
}

static void zero_wait_is_one_poll(void) {
    uint32_t s[4];
    bool     m[4];
    CHECK(slices(0, s, m, 4) == 1 && s[0] == 0 && !m[0]);
}

static void short_waits_are_one_slice(void) {
    uint32_t const t[] = {1, 30, 49, 50};
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        uint32_t s[4];
        bool     m[4];
        CHECK(slices(t[i], s, m, 4) == 1 && s[0] == t[i] && !m[0]);
    }
}

static void longer_waits_add_up(void) {
    uint32_t s[8];
    bool     m[8];
    CHECK(slices(51, s, m, 8) == 2 && s[0] == 50 && m[0] && s[1] == 1 && !m[1]);
    CHECK(slices(120, s, m, 8) == 3 && s[0] == 50 && s[1] == 50 && s[2] == 20 && m[0] && m[1] && !m[2]);
    uint32_t big[200];
    bool     bm[200];
    unsigned n   = slices(5000, big, bm, 200);
    uint32_t sum = 0;
    for (unsigned i = 0; i < n; i++) {
        sum += big[i];
    }
    CHECK(n == 100 && sum == 5000 && !bm[n - 1]);
}

// Forever is sliced too (a dead C6 must end it) and never runs out
static void forever_is_sliced_and_endless(void) {
    uint32_t left = EH_RPC_WAIT_FOREVER;
    for (int i = 0; i < 1000; i++) {
        bool more = false;
        CHECK(eh_rpc_next_slice(&left, POLL, &more) == POLL && more);
    }
    CHECK(left == EH_RPC_WAIT_FOREVER);
}

int main(void) {
    starts_not_failed();
    failure_latches();
    reattach_clears();
    rx_clears();
    zero_wait_is_one_poll();
    short_waits_are_one_slice();
    longer_waits_add_up();
    forever_is_sliced_and_endless();
    if (failures) {
        printf("hosted_link_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("hosted_link_test: all passed\n");
    return 0;
}
