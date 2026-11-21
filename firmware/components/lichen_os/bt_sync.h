#pragma once

#include "reg_proxy.h"

#include <stdbool.h>
#include <stdint.h>

// Keeping the C6's Bluetooth controller at what the OS radio switch asks for (#503), pure and host-tested
// (bt_sync_test.c). lichen_reg.c feeds it the registry's state of bt.enable and posts what it answers; the switch and
// airplane mode have already been folded into one wish by the radio state (radio_state_effective), so the mapping is
// "the wish is bt.enable".
//
// - An old C6 (no registry, or no Bluetooth domain, or one that answers UNKNOWN for bt.enable) is never sent anything:
//   the answer is UNSUPPORTED, once, and the Settings row says "not supported by this C6".
// - A SET goes out at most BT_SYNC_TRIES times per wish and per link, BT_SYNC_RETRY_MS apart, then GIVE_UP: a C6 that
//   keeps refusing is not asked in a loop.
#define BT_SYNC_TRIES    3
#define BT_SYNC_RETRY_MS 5000

typedef struct {
    bool         want;          // the OS's effective Bluetooth state (switch, airplane mode and holds)
    rp_support_t support;       // reg_proxy_support for bt.enable
    bool         applied_known; // an answer for bt.enable since the link came up (not stale)
    bool         applied_on;    // that answer: the controller is enabled
    bool         unknown_id;    // the C6 answered UNKNOWN for bt.enable
    uint8_t      tries;         // SETs sent for this wish on this link
    uint32_t     since_sent_ms; // since the last of them (only read when tries > 0)
} bt_sync_in_t;

typedef enum {
    BT_SYNC_IDLE,        // the C6 already is what is wanted
    BT_SYNC_SEND,        // post REG_SET bt.enable = want
    BT_SYNC_WAIT,        // caps not known yet, or a SET just went out: look again later
    BT_SYNC_UNSUPPORTED, // this C6 has no bt.enable
    BT_SYNC_GIVE_UP,     // BT_SYNC_TRIES sent, the C6 still is not what is wanted
} bt_sync_act_t;

bt_sync_act_t bt_sync_decide(bt_sync_in_t const *in);

// The bookkeeping lichen_reg.c keeps under its lock: the wish, the SETs sent for it and the one log line. All pure, so
// the wish change, the link reset and the slot-to-input mapping are tested with a real reg_proxy_t.
typedef struct {
    bool     asked;   // main has told the wish at least once
    bool     want;
    bool     said;    // the one log line (unsupported or give up) for this wish and link is written
    uint8_t  tries;
    uint32_t sent_ms;
} bt_sync_t;

// A wish: a new value (or the first one) starts the count and the log line again
void bt_sync_wish(bt_sync_t *s, bool on);
// The C6 link changed (it may be reset or updated): the count and the log line start again
void bt_sync_link(bt_sync_t *s);
// What the registry knows about bt.enable (slot, may be NULL; support from reg_proxy_support) as a decision input. An
// UNKNOWN answer counts only while it is not stale, so a C6 updated to a build with the key is asked again after the link
// came back.
bt_sync_in_t bt_sync_input(bt_sync_t const *s, reg_proxy_slot_t const *slot, rp_support_t support, uint32_t now_ms);
// bt_sync_decide on that input; IDLE until a wish has been told
bt_sync_act_t bt_sync_step(bt_sync_t const *s, reg_proxy_slot_t const *slot, rp_support_t support, uint32_t now_ms);
// An explicit set (the console's fw set bt.enable) after a give-up: the count starts again so it is tried, unless a SET
// is still in flight
void bt_sync_retry(bt_sync_t *s, uint32_t now_ms);
// A SET went out
void bt_sync_sent(bt_sync_t *s, uint32_t now_ms);
// True once per wish and link: the caller writes its log line now
bool bt_sync_say(bt_sync_t *s);
// A SET went out less than BT_SYNC_RETRY_MS ago: its result may still come (the mismatch warning waits for it)
bool bt_sync_in_flight(bt_sync_t const *s, uint32_t now_ms);
