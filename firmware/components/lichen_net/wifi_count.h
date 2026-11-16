#pragma once

// Wi-Fi link-loss bookkeeping (#415) for `fw soak`: how often an established link was lost, how often it came back and
// why the last disconnect happened. Numbers only, no network name. Pure (no ESP-IDF headers): wifi_service.c feeds it
// from the c6wifi task and reads it under its state lock; host_tests/wifi_count_test.c pins the rules.

#include <stdbool.h>
#include <stdint.h>

// WIFI_REASON_ASSOC_LEAVE of ESP-IDF: what esp_wifi_disconnect() (a join, a forget, a rollback) reports back. A
// disconnect the owner or the OS asked for is not a lost link, so it is not counted.
#define WIFI_COUNT_REASON_LOCAL 8

typedef struct {
    uint32_t disconnects;  // links that were up (had an IP) and were lost, not asked for, since boot
    uint32_t reconnects;   // got-IPs after such a loss
    uint16_t last_reason;  // IDF reason of the newest disconnect event that was not asked for; 0: none yet
    bool     up;           // an IP since the last disconnect
    bool     lost;         // a counted loss has not got its reconnect yet
} wifi_count_t;

void wifi_count_init(wifi_count_t *c);
// The link got an IP. Counts a reconnect when a counted loss is waiting for one.
void wifi_count_got_ip(wifi_count_t *c);
// A disconnect event with its IDF reason. Only a link that was up counts as a loss; every failed attempt of a retry
// loop still updates last_reason (it says why the link stays down).
void wifi_count_disconnected(wifi_count_t *c, int reason);
// The link went away with no disconnect event (the C6 went down for a re-attach): an up link counts as a loss, the
// reason stays. The got-IP after the re-attach is then its reconnect.
void wifi_count_link_gone(wifi_count_t *c);
// The owner switched Wi-Fi off: not a loss, whatever reason the driver's stop event carries.
void wifi_count_owner_down(wifi_count_t *c);
