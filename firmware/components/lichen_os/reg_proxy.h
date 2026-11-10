#pragma once

#include "reg_cmd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The P4's copy of the keys the C6 serves (#310): one slot per proxied key with the last REG_VAL, its age and the seq
// it answered, plus what c6.reg_caps said. Pure, host-tested (reg_service_test.c). lichen_reg.c keeps one under a
// spinlock: the LoRa task writes answers, the console and the screens read; no call here waits or calls out.
//
// - seq: every request gets the next one; only the answer to the newest request of a key counts (a late answer to an
//   older one is dropped), so a slow REG_VAL is never taken for the next request's.
// - c6.reg_caps is asked first: no answer within REG_PROXY_CAPS_TIMEOUT_MS = a C6 without the registry (an old C6
//   ignores REG_GET as BAD_TYPE), shown "not supported by this C6"; a missing domain bit = keys of that domain are not
//   asked at all.
// - A C6 that went away or came back (reg_proxy_link): caps are asked anew, every value is stale until answered again.
// - requested vs applied: one W line per key and boot when they differ after both held still for REG_PROXY_SETTLE_MS.
#define REG_PROXY_MAX             16 // 14 used with #311 (bl 4, lora 8, bt.state, c6.reg_caps); reg_service_test checks
#define REG_PROXY_CAPS_TIMEOUT_MS 1000
#define REG_PROXY_CAPS_TRIES      3 // background re-asks of c6.reg_caps after a boot or a re-attach; the console always asks
#define REG_PROXY_SETTLE_MS       3000

typedef struct {
    uint16_t id;
    uint8_t  type;   // the P4 table's reg_type_t: an OK answer of another type is dropped
    uint8_t  status; // reg_status_t of the last answer
    uint8_t  seq;    // of the newest request
    bool     have, pending, stale, warned, req_known;
    uint32_t value, at_ms, sent_ms;
    // The request before the newest, so a cancelled post gives the one still in flight its seq back
    uint8_t  prev_seq;
    bool     prev_pending;
    uint32_t prev_sent_ms;
    uint32_t req_value, req_since_ms;
} reg_proxy_slot_t;

typedef struct {
    reg_proxy_slot_t slot[REG_PROXY_MAX];
    size_t           n;
    uint8_t          next_seq;
    uint8_t          caps_tries;
    uint32_t         link_ms; // boot or the last re-attach
    uint32_t         late, bad_type;
} reg_proxy_t;

typedef enum { RP_OK, RP_WAIT_CAPS, RP_OLD_C6, RP_NO_DOMAIN } rp_support_t;
typedef enum { RP_ACCEPTED, RP_LATE, RP_BAD_TYPE, RP_NOT_OURS } rp_answer_t;

// ids/types of the proxied keys; c6.reg_caps (REG_ID_C6_REG_CAPS) must be one of them. False when n is too large.
bool reg_proxy_init(reg_proxy_t *p, uint16_t const *ids, uint8_t const *types, size_t n, uint32_t now_ms);
reg_proxy_slot_t const *reg_proxy_slot(reg_proxy_t const *p, uint16_t id);

// May the key be asked now? c6.reg_caps always may.
rp_support_t reg_proxy_support(reg_proxy_t const *p, uint16_t id, uint32_t now_ms);
// The caps are known (answered since the last link change)
bool reg_proxy_caps_known(reg_proxy_t const *p);
// The background should ask c6.reg_caps (not known, not just asked, tries left)
bool reg_proxy_caps_due(reg_proxy_t const *p, uint32_t now_ms);

// A request for id goes out: *seq to send with it. False for an id that is not proxied.
bool reg_proxy_begin(reg_proxy_t *p, uint16_t id, uint32_t now_ms, uint8_t *seq);
// The request could not be posted (no link): the key goes back to the request before it (a GET still in flight keeps
// its seq and stays answerable) and a c6.reg_caps try is not counted
void reg_proxy_cancel(reg_proxy_t *p, uint16_t id, uint8_t seq);
bool reg_proxy_pending(reg_proxy_t const *p, uint16_t id);
rp_answer_t reg_proxy_answer(reg_proxy_t *p, uint16_t id, uint8_t seq, uint8_t status, uint8_t type, uint32_t value,
                             uint32_t now_ms);
void reg_proxy_link(reg_proxy_t *p, uint32_t now_ms);

// Note what the P4 asks for now; true once per key when the applied value does not match it (reg_applied_matches)
// after both held still for the settle time
bool reg_proxy_mismatch(reg_proxy_t *p, uint16_t id, reg_value_t const *requested, uint32_t now_ms);

// The view's applied part for a proxied key. live: a live ask just ended, so a request still pending means no answer
// came. no_link: the ask could not be sent.
void reg_proxy_view(reg_proxy_t const *p, uint16_t id, uint32_t now_ms, bool live, bool no_link, reg_view_t *v);
