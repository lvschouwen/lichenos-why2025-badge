#pragma once

#include <stdbool.h>
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

typedef struct {
    lora_regq_req_t req[LORA_REGQ_MAX];
    uint8_t         head, count;
    uint16_t        blocked[LORA_REGQ_BLOCK_MAX];
    uint8_t         n_blocked;
    bool            wake_pending; // a wake token sits in the LoRa task's receive queue
} lora_regq_t;

// Drops every waiting entry; blocked ids stay blocked
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

// The oldest entry, taken out; false when the queue is empty
bool lora_regq_take(lora_regq_t *q, lora_regq_req_t *out);
