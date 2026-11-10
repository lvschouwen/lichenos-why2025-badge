#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The registry requests waiting for the LoRa task (#310): REG_GET / REG_SET to the C6, posted from any task without
// blocking (a backlight slider on the LVGL task included) and sent by the LoRa task, which owns the peer-data sends.
// Pure, host-tested (lora_regq_test.c); lora.c keeps one under its state lock.
//
// One entry per key id at most: a newer request for an id that still waits takes over that entry and its place in
// line. A SET replaces the value (the newest level wins, so a slider never queues up stale steps) and turns a waiting
// GET into a SET; a GET keeps a waiting SET (its REG_VAL carries the applied value as well). Either way the entry gets
// the newer seq, the one the registry waits for. So a queue of LORA_REGQ_MAX never fills while the registry has fewer
// keys than that.
#define LORA_REGQ_MAX 16

typedef struct {
    uint16_t id;
    uint32_t value;
    uint8_t  seq;
    bool     set; // REG_SET with value, else REG_GET
} lora_regq_req_t;

// Ids closed for good (lora_regq_block): at a restart the backlight's REG_SETs must not reach the C6 after the UART
// blank (#310), so their waiting entries are dropped and new posts refused
#define LORA_REGQ_BLOCK_MAX 4

// Requests in flight (#314): the C6 answers every REG_GET / REG_SET with a REG_VAL, back to back, into the LoRa task's
// 8-deep receive queue, which also carries RX_PACKET. A request goes out only when the messages already waiting there,
// the answers still to come (one per unanswered request), its own answer and one wake token leave LORA_REGQ_RX_ROOM
// places free; and never more than LORA_REGQ_IN_FLIGHT unanswered. A request counts until its answer comes, or
// LORA_REGQ_ANSWER_MS after it went out (the C6 answers within milliseconds, its radio TX does not hold it up; past
// that a silent or old C6 must not wedge the queue): only an answer later than that can use one of the free places.
#define LORA_REGQ_IN_FLIGHT 4
#define LORA_REGQ_ANSWER_MS 1000
#define LORA_REGQ_RX_ROOM   3

typedef struct {
    uint16_t id;
    uint8_t  seq;
    uint32_t sent_ms;
} lora_regq_fly_t;

typedef struct {
    lora_regq_req_t req[LORA_REGQ_MAX];
    uint8_t         head, count;
    uint16_t        blocked[LORA_REGQ_BLOCK_MAX];
    uint8_t         n_blocked;
    bool            wake_pending; // a wake token sits in the LoRa task's receive queue
    lora_regq_fly_t fly[LORA_REGQ_IN_FLIGHT];
    uint8_t         n_fly;
    uint32_t        timeouts; // requests forgotten without an answer
} lora_regq_t;

// Drops every waiting entry and forgets the requests in flight; blocked ids stay blocked
void lora_regq_clear(lora_regq_t *q);
// Drops id's waiting entry (the others keep their order) and refuses every later post for it. False when the block
// list is full (the entry is dropped all the same).
bool lora_regq_block(lora_regq_t *q, uint16_t id);
// False when the queue is full and no entry for this id waits, or the id is blocked
bool lora_regq_post(lora_regq_t *q, lora_regq_req_t const *req);
// Wake tokens (#310 review): at most one in the LoRa task's 8-deep receive queue, so a burst of posts never crowds out
// RX_PACKET or REG_VAL. want_wake: true = send a token now (and it counts as outstanding); woken: the task is about to
// drain the queue, so the next post needs a token again (also when sending the token failed).
bool lora_regq_want_wake(lora_regq_t *q);
void lora_regq_woken(lora_regq_t *q);

// The restart blank's pause (#310 review): a REG_SET that left within LORA_REGQ_SETTLE_RECENT_MS may still be applied
// by the C6 after the caller's next UART line, so the caller waits LORA_REGQ_SETTLE_MS first; 0 otherwise
#define LORA_REGQ_SETTLE_RECENT_MS 100
#define LORA_REGQ_SETTLE_MS        20
uint32_t lora_regq_settle_ms(bool set_sent, uint32_t set_sent_ms, uint32_t now_ms);

// The oldest entry, taken out and counted in flight from now_ms; false when the queue is empty, LORA_REGQ_IN_FLIGHT
// requests are unanswered, or the receive queue (rx_depth places, rx_waiting taken) has no room for its answer as
// above. Requests older than LORA_REGQ_ANSWER_MS are forgotten first.
bool lora_regq_take(lora_regq_t *q, uint32_t now_ms, size_t rx_waiting, size_t rx_depth, lora_regq_req_t *out);
// The taken request id/seq left for the C6 at now_ms: its timeout runs from here, not from the take before the send
void lora_regq_sent(lora_regq_t *q, uint16_t id, uint8_t seq, uint32_t now_ms);
// A REG_VAL for id/seq came (or that request could not be sent): its slot is free. False when none matched (a late
// answer after its timeout, or one nobody asked for).
bool lora_regq_answered(lora_regq_t *q, uint16_t id, uint8_t seq);
// How long the LoRa task may wait for its receive queue: wait_ms, or less when requests wait while some are unanswered
// and the oldest of those is forgotten sooner (no answer then wakes the task)
uint32_t lora_regq_wait_ms(lora_regq_t const *q, uint32_t now_ms, uint32_t wait_ms);
// Requests waiting or unanswered: the ones a request posted now goes out behind (itself included once posted)
size_t lora_regq_ahead(lora_regq_t const *q);
// The longest n requests take to go out when every window of LORA_REGQ_IN_FLIGHT before the last stays unanswered,
// on top of the C6's own answer time (the console's live wait)
uint32_t lora_regq_burst_ms(size_t n);
