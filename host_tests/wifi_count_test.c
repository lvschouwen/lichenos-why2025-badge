// Host test: the Wi-Fi link-loss counters of `fw soak` (#415)
#include "wifi_count.h"

#include <stdio.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void fresh(void) {
    wifi_count_t c;
    wifi_count_init(&c);
    CHECK(c.disconnects == 0 && c.reconnects == 0 && c.last_reason == 0);
}

// A link that was up, lost and came back: one of each, and the reason of the loss
static void loss_and_return(void) {
    wifi_count_t c;
    wifi_count_init(&c);
    wifi_count_got_ip(&c);
    CHECK(c.reconnects == 0); // the first IP is not a reconnect
    wifi_count_disconnected(&c, 200);
    CHECK(c.disconnects == 1 && c.last_reason == 200);
    wifi_count_got_ip(&c);
    CHECK(c.disconnects == 1 && c.reconnects == 1);
}

// A retry loop against an absent network: one loss, the failed attempts only update the reason
static void outage_counts_once(void) {
    wifi_count_t c;
    wifi_count_init(&c);
    wifi_count_got_ip(&c);
    wifi_count_disconnected(&c, 4);
    for (int i = 0; i < 20; i++) {
        wifi_count_disconnected(&c, 201);
    }
    CHECK(c.disconnects == 1 && c.reconnects == 0 && c.last_reason == 201);
    wifi_count_got_ip(&c);
    wifi_count_got_ip(&c); // a repeated IP event is not another reconnect
    CHECK(c.disconnects == 1 && c.reconnects == 1);
}

// Flapping: each drop and return counts
static void flapping(void) {
    wifi_count_t c;
    wifi_count_init(&c);
    for (int i = 0; i < 3; i++) {
        wifi_count_got_ip(&c);
        wifi_count_disconnected(&c, 2);
    }
    CHECK(c.disconnects == 3 && c.reconnects == 2);
}

// A join, forget or rollback disconnects by itself (reason ASSOC_LEAVE): not a loss, and the reason is not kept
static void asked_for_is_not_counted(void) {
    wifi_count_t c;
    wifi_count_init(&c);
    wifi_count_got_ip(&c);
    wifi_count_disconnected(&c, 201);
    wifi_count_got_ip(&c);
    wifi_count_disconnected(&c, WIFI_COUNT_REASON_LOCAL);
    CHECK(c.disconnects == 1 && c.last_reason == 201);
    wifi_count_got_ip(&c);
    CHECK(c.reconnects == 1); // the new network's IP is not a reconnect either
    // A loss waiting for its reconnect, then the owner joins another network: that IP is not its reconnect
    wifi_count_disconnected(&c, 3);
    wifi_count_disconnected(&c, WIFI_COUNT_REASON_LOCAL);
    wifi_count_got_ip(&c);
    CHECK(c.disconnects == 2 && c.reconnects == 1);
    // After the owner's disconnect, a failed attempt of the new network is no loss of an up link
    wifi_count_disconnected(&c, WIFI_COUNT_REASON_LOCAL);
    wifi_count_disconnected(&c, 15);
    CHECK(c.disconnects == 2 && c.last_reason == 15);
}

// A disconnect before any IP (a wrong password, no AP) is no lost link
static void never_up(void) {
    wifi_count_t c;
    wifi_count_init(&c);
    wifi_count_disconnected(&c, 15);
    wifi_count_disconnected(&c, 205);
    CHECK(c.disconnects == 0 && c.last_reason == 205);
    wifi_count_disconnected(&c, -3);
    CHECK(c.last_reason == 0);
}

int main(void) {
    fresh();
    loss_and_return();
    outage_counts_once();
    flapping();
    asked_for_is_not_counted();
    never_up();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("wifi_count_test: all passed\n");
    return 0;
}
