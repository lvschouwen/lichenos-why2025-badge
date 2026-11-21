#pragma once

#include "wifi_join.h"

#include <stdbool.h>
#include <stdint.h>

// Requests to the c6wifi task (#502): scan, power save, TX power and protocol/bandwidth reach the driver only on that
// task, so a caller posts a message and waits a bounded time for the answer. This is the pure part (host-tested): the
// answer slot, with a sequence number so that a caller that gave up never reads, and the task never delivers, the
// answer of a request that belongs to someone else; and the scan gate.

typedef enum {
    WIFI_REQ_OK,
    WIFI_REQ_UPDATING,   // the C6 is being updated
    WIFI_REQ_NO_LINK,    // no C6 link
    WIFI_REQ_OFF,        // the Wi-Fi switch is off
    WIFI_REQ_DRIVER,     // the driver is not up (starting or re-attaching)
    WIFI_REQ_JOINING,    // a trial join is in flight
    WIFI_REQ_ROAMING,    // a roam is in flight
    WIFI_REQ_BUSY,       // another request holds the task (the wait ran out)
    WIFI_REQ_FAILED,     // the driver said no
    WIFI_REQ_BAD_ARG,    // the value is not one the setter takes
} wifi_req_why_t;

typedef struct {
    uint32_t       seq;      // the request that owns the slot; 0 = none yet
    uint32_t       done_seq; // the request the task has answered
    bool           ok;
    wifi_req_why_t why;
} wifi_req_t;

void wifi_req_init(wifi_req_t *r);
// A new request owns the slot from now on: the answer of an older one is dropped. Returns its number (never 0).
uint32_t wifi_req_begin(wifi_req_t *r);
// The task answers request seq; false (and nothing stored) when the slot belongs to a newer one
bool wifi_req_finish(wifi_req_t *r, uint32_t seq, bool ok, wifi_req_why_t why);
// The caller asks: true when request seq is answered (ok and why filled)
bool wifi_req_result(wifi_req_t const *r, uint32_t seq, bool *ok, wifi_req_why_t *why);
// The task, before it starts a long request: false when its caller has gone on to a newer one
bool wifi_req_wanted(wifi_req_t const *r, uint32_t seq);

// Whether a scan may start. roaming: the C6 left an AP for a better one and has not arrived yet (#495).
wifi_req_why_t wifi_scan_gate(bool link_up, bool updating, bool radio_on, bool driver_up, wifi_join_state_t join, bool roaming);
// A protocol or bandwidth change needs the association to be left alone, like a scan: refused during a join or a roam
wifi_req_why_t wifi_link_gate(wifi_join_state_t join, bool roaming);

// What the task does with request seq once it reaches it (#502 review): run = do the work; answer = tell the caller.
// A request whose caller has gone (timed out, or a newer request owns the slot) is skipped silently, so a timeout plus a
// retry never runs the same reconnect twice. gate: the refusal that applies now (WIFI_REQ_OK = none).
typedef struct {
    bool           run;
    bool           answer;
    wifi_req_why_t why; // the refusal, when answer && !run
} wifi_req_plan_t;
wifi_req_plan_t wifi_req_plan(wifi_req_t const *r, uint32_t seq, wifi_req_why_t gate);
// The value a choice keeps after the task tried to apply it: the new one only when the driver took it
int wifi_req_keep(bool applied, int old_value, int new_value);
char const    *wifi_req_why_text(wifi_req_why_t why);
