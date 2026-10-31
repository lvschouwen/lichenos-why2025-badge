// Host test: the trial join state machine (#66): new credentials are kept only once they got an IP
#include "wifi_join.h"

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
    wifi_join_t j;
    wifi_join_init(&j);
    CHECK(j.state == WIFI_JOIN_NONE);
    // Nothing is decided without a trial
    CHECK(wifi_join_event(&j, JOIN_EV_GOT_IP, 0, 100) == JOIN_ACT_NONE && j.state == WIFI_JOIN_NONE);
    CHECK(wifi_join_event(&j, JOIN_EV_TICK, 0, 99999) == JOIN_ACT_NONE);

    // Success: an IP commits once
    wifi_join_start(&j, 1000);
    CHECK(j.state == WIFI_JOIN_TRYING);
    CHECK(wifi_join_event(&j, JOIN_EV_DISCONNECTED, 201, 2000) == JOIN_ACT_NONE); // AP not seen yet: keep trying
    CHECK(wifi_join_event(&j, JOIN_EV_GOT_IP, 0, 3000) == JOIN_ACT_COMMIT && j.state == WIFI_JOIN_JOINED);
    CHECK(wifi_join_event(&j, JOIN_EV_GOT_IP, 0, 3100) == JOIN_ACT_NONE);
    CHECK(wifi_join_event(&j, JOIN_EV_TICK, 0, 90000) == JOIN_ACT_NONE && j.state == WIFI_JOIN_JOINED);

    // Timeout: 20 s without an IP rolls back
    wifi_join_start(&j, 5000);
    CHECK(wifi_join_event(&j, JOIN_EV_TICK, 0, 24999) == JOIN_ACT_NONE);
    CHECK(wifi_join_event(&j, JOIN_EV_TICK, 0, 25000) == JOIN_ACT_ROLLBACK && j.state == WIFI_JOIN_FAILED_KEPT_OLD);
    CHECK(wifi_join_event(&j, JOIN_EV_TICK, 0, 26000) == JOIN_ACT_NONE); // decided once

    // Authentication failure rolls back at once
    wifi_join_start(&j, 0);
    CHECK(wifi_join_event(&j, JOIN_EV_DISCONNECTED, 8, 10) == JOIN_ACT_NONE); // our own disconnect
    CHECK(wifi_join_event(&j, JOIN_EV_DISCONNECTED, 15, 20) == JOIN_ACT_ROLLBACK);
    CHECK(j.state == WIFI_JOIN_FAILED_KEPT_OLD);
    CHECK(wifi_reason_is_auth_failure(14) && wifi_reason_is_auth_failure(15) && wifi_reason_is_auth_failure(202) &&
          wifi_reason_is_auth_failure(204));
    CHECK(!wifi_reason_is_auth_failure(8) && !wifi_reason_is_auth_failure(201) && !wifi_reason_is_auth_failure(0));
    // A failed join after the trial is over changes nothing
    CHECK(wifi_join_event(&j, JOIN_EV_DISCONNECTED, 15, 30) == JOIN_ACT_NONE);

    // The tick counter may wrap during a trial
    wifi_join_start(&j, 0xFFFFFF00u);
    CHECK(wifi_join_event(&j, JOIN_EV_TICK, 0, 1000) == JOIN_ACT_NONE);
    CHECK(wifi_join_event(&j, JOIN_EV_TICK, 0, 0xFFFFFF00u + WIFI_JOIN_TRIAL_MS) == JOIN_ACT_ROLLBACK);

    // A request that the task has not started yet decides nothing: events of the old connection are ignored
    wifi_join_init(&j);
    wifi_join_request(&j);
    CHECK(j.state == WIFI_JOIN_PENDING);
    CHECK(wifi_join_event(&j, JOIN_EV_GOT_IP, 0, 10) == JOIN_ACT_NONE && j.state == WIFI_JOIN_PENDING);
    CHECK(wifi_join_event(&j, JOIN_EV_DISCONNECTED, 15, 20) == JOIN_ACT_NONE && j.state == WIFI_JOIN_PENDING);
    CHECK(wifi_join_event(&j, JOIN_EV_TICK, 0, 99999) == JOIN_ACT_NONE && j.state == WIFI_JOIN_PENDING);
    wifi_join_start(&j, 100);
    CHECK(j.state == WIFI_JOIN_TRYING);
    CHECK(wifi_join_event(&j, JOIN_EV_GOT_IP, 0, 200) == JOIN_ACT_COMMIT);
    // A request over a running trial also pauses it until the new one starts
    wifi_join_start(&j, 0);
    wifi_join_request(&j);
    CHECK(wifi_join_event(&j, JOIN_EV_TICK, 0, 50000) == JOIN_ACT_NONE);

    // An IP from another network (a late GOT_IP of the old connection) commits nothing: the trial goes on
    CHECK(wifi_join_ip_event("New", "Old") == JOIN_EV_GOT_IP_OTHER);
    CHECK(wifi_join_ip_event("New", "New") == JOIN_EV_GOT_IP);
    CHECK(wifi_join_ip_event("New", "new") == JOIN_EV_GOT_IP_OTHER);
    CHECK(wifi_join_ip_event("New", NULL) == JOIN_EV_GOT_IP_OTHER); // AP unknown: not proven
    CHECK(wifi_join_ip_event("", "") == JOIN_EV_GOT_IP_OTHER);       // no trial SSID: nothing to prove
    wifi_join_init(&j);
    wifi_join_start(&j, 0);
    CHECK(wifi_join_event(&j, JOIN_EV_GOT_IP_OTHER, 0, 100) == JOIN_ACT_NONE && j.state == WIFI_JOIN_TRYING);
    CHECK(wifi_join_event(&j, JOIN_EV_GOT_IP, 0, 200) == JOIN_ACT_COMMIT && j.state == WIFI_JOIN_JOINED);
    wifi_join_start(&j, 0);
    CHECK(wifi_join_event(&j, JOIN_EV_GOT_IP_OTHER, 0, 100) == JOIN_ACT_NONE);
    CHECK(wifi_join_event(&j, JOIN_EV_TICK, 0, WIFI_JOIN_TRIAL_MS) == JOIN_ACT_ROLLBACK); // and it still times out

    if (failures) {
        return 1;
    }
    printf("wifi_join_test: ok\n");
    return 0;
}
