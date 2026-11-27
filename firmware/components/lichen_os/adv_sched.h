#pragma once

#include <stdbool.h>
#include <stdint.h>

// When and how fast the OS advertises (#73, #599), pure and host-tested (adv_sched_test.c); not linked into the image
// until the GAP arbiter (#73 B2) uses it. Advertising runs only while some client asks for it and one more peer may
// connect (ble_conn_may_advertise). A new ask (or a restart: a link went down, the pairing window opened) starts a
// round: fast (ADV_FAST_ITVL_MS) for ADV_FAST_MS, then slow (ADV_SLOW_ITVL_MS) for ADV_SLOW_MS, then off until the next
// round. When every ask is only for bonded peers to come back, the round is slow only: a known phone finds the badge
// within a second or two, and the radio stays quiet. The connection parameters the OS asks a peer for are here too.

#define ADV_FAST_ITVL_MS 100u
#define ADV_FAST_MS      30000u
#define ADV_SLOW_ITVL_MS 1000u
#define ADV_SLOW_MS      300000u // then off: a round lasts 5.5 min (5 min when slow only)

// Connection parameters, in the spec's units: interval 1.25 ms (30-50 ms), latency in events, timeout 10 ms (5 s)
#define BT_CONN_ITVL_MIN    24u
#define BT_CONN_ITVL_MAX    40u
#define BT_CONN_LATENCY_MIN 0u
#define BT_CONN_LATENCY_MAX 4u
#define BT_CONN_SUPERVISION 500u

typedef enum { ADV_OFF, ADV_FAST, ADV_SLOW } adv_mode_t;

typedef struct {
    bool want;        // some client asks for advertising
    bool bonded_only; // every ask is only for bonded peers to reconnect
    bool room;        // one more peer may connect (the connection cap)
} adv_wish_t;

typedef struct {
    bool     asked; // a round is under way (want was true at the last step)
    uint32_t start_ms;
} adv_sched_t;

void       adv_sched_init(adv_sched_t *s);
// A new reason to be found (a link dropped, the pairing window opened): the next step starts a fresh round
void       adv_sched_restart(adv_sched_t *s);
// What to advertise now; a want that turns true starts a round, one that turns false ends it
adv_mode_t adv_sched_step(adv_sched_t *s, uint32_t now_ms, adv_wish_t w);
// The advertising interval for mode, 0 for off
uint32_t   adv_sched_interval_ms(adv_mode_t mode);
// How long until the mode can change by itself (the caller's next timer), 0 when it will not
uint32_t   adv_sched_next_ms(adv_sched_t const *s, uint32_t now_ms, adv_wish_t w);
