// Host tests for the LoRa task's registry request queue (#310): order, one entry per id (a newer SET wins, a GET keeps
// a waiting SET), the newest seq, a full queue, the restart block (no backlight REG_SET after the UART blank), and the
// in-flight window that keeps the REG_VAL answers from overflowing the receive queue (#314)
#include "lora_regq.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static bool post(lora_regq_t *q, uint16_t id, bool set, uint32_t value, uint8_t seq) {
    lora_regq_req_t const r = {.id = id, .value = value, .seq = seq, .set = set};
    return lora_regq_post(q, &r);
}

// A take whose answer comes at once: these tests are about order, not the in-flight window
static bool take(lora_regq_t *q, lora_regq_req_t *r) {
    bool ok = lora_regq_take(q, 0, 0, 8, r);
    if (ok) {
        CHECK(lora_regq_answered(q, r->id, r->seq));
    }
    return ok;
}

static void test_order_and_coalesce(void) {
    lora_regq_t     q = {0};
    lora_regq_req_t r;
    lora_regq_clear(&q);
    CHECK(!take(&q, &r));
    CHECK(post(&q, 0x0100, true, 40, 1));
    CHECK(post(&q, 0x0101, true, 10, 2));
    CHECK(post(&q, 0x0100, true, 45, 3)); // a slider step: replaces, keeps its place
    CHECK(post(&q, 0x0100, false, 0, 4)); // a GET after it: the SET stays, with the newer seq
    CHECK(post(&q, 0x0200, false, 0, 5));
    CHECK(q.count == 3);
    CHECK(take(&q, &r) && r.id == 0x0100 && r.set && r.value == 45 && r.seq == 4);
    CHECK(take(&q, &r) && r.id == 0x0101 && r.set && r.value == 10 && r.seq == 2);
    CHECK(post(&q, 0x0200, true, 7, 6)); // a SET over a waiting GET turns it into the SET
    CHECK(take(&q, &r) && r.id == 0x0200 && r.set && r.value == 7 && r.seq == 6);
    CHECK(!take(&q, &r));
}

static void test_full(void) {
    lora_regq_t     q = {0};
    lora_regq_req_t r;
    lora_regq_clear(&q);
    for (uint16_t i = 0; i < LORA_REGQ_MAX; i++) {
        CHECK(post(&q, i, false, 0, (uint8_t)i));
    }
    CHECK(!post(&q, 0x0F00, false, 0, 99)); // full, and no entry for that id
    CHECK(post(&q, 3, true, 1, 100));       // an id that waits always gets through
    for (uint16_t i = 0; i < 5; i++) {
        CHECK(take(&q, &r) && r.id == i);
    }
    CHECK(post(&q, 0x0F00, false, 0, 101)); // wraps around
    for (uint16_t i = 5; i < LORA_REGQ_MAX; i++) {
        CHECK(take(&q, &r) && r.id == i);
    }
    CHECK(take(&q, &r) && r.id == 0x0F00 && r.seq == 101);
    CHECK(!take(&q, &r));
}

// The restart blank (#310): the backlight's waiting REG_SETs are dropped, later ones refused, the rest keep their order
static void test_block(void) {
    lora_regq_t     q = {0};
    lora_regq_req_t r;
    lora_regq_clear(&q);
    CHECK(post(&q, 0x0200, false, 0, 1));
    CHECK(post(&q, 0x0100, true, 40, 2));
    CHECK(post(&q, 0x0F00, false, 0, 3));
    CHECK(post(&q, 0x0101, true, 10, 4));
    CHECK(lora_regq_block(&q, 0x0100));
    CHECK(lora_regq_block(&q, 0x0101));
    CHECK(lora_regq_block(&q, 0x0101)); // twice is fine
    CHECK(q.count == 2);
    CHECK(!post(&q, 0x0100, true, 0, 5)); // refused for good
    CHECK(!post(&q, 0x0101, false, 0, 6));
    lora_regq_clear(&q); // a C6 loss drops the queue, the block stays
    CHECK(!post(&q, 0x0100, true, 0, 7));
    CHECK(post(&q, 0x0200, false, 0, 8));
    CHECK(take(&q, &r) && r.id == 0x0200 && r.seq == 8);
    CHECK(!take(&q, &r));
    // Order of the others across a wrapped ring
    lora_regq_t w = {0};
    for (uint16_t i = 0; i < LORA_REGQ_MAX; i++) {
        CHECK(post(&w, (uint16_t)(0x300 + i), false, 0, (uint8_t)i));
    }
    for (int i = 0; i < 10; i++) {
        CHECK(take(&w, &r));
    }
    for (uint16_t i = 0; i < 6; i++) {
        CHECK(post(&w, (uint16_t)(0x400 + i), false, 0, 0));
    }
    CHECK(lora_regq_block(&w, 0x30C));
    CHECK(lora_regq_block(&w, 0x401));
    uint16_t const want[] = {0x30A, 0x30B, 0x30D, 0x30E, 0x30F, 0x400, 0x402, 0x403, 0x404, 0x405};
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        CHECK(take(&w, &r) && r.id == want[i]);
    }
    CHECK(!take(&w, &r));
    // The block list has room for the backlight's ids and a few more, no more
    lora_regq_t f = {0};
    for (uint16_t i = 0; i < LORA_REGQ_BLOCK_MAX; i++) {
        CHECK(lora_regq_block(&f, i));
    }
    CHECK(!lora_regq_block(&f, 0x0F00));
}

// One wake token at most in the LoRa task's receive queue (#310 review M2)
static void test_wake(void) {
    lora_regq_t     q = {0};
    lora_regq_req_t r;
    CHECK(post(&q, 0x0100, true, 40, 1) && lora_regq_want_wake(&q));
    for (uint16_t i = 0; i < 20; i++) { // a poll burst: no more tokens
        CHECK(!post(&q, (uint16_t)(0x0200 + i), false, 0, 2) || !lora_regq_want_wake(&q));
    }
    lora_regq_woken(&q); // the task drains
    while (take(&q, &r)) {
    }
    CHECK(post(&q, 0x0101, true, 10, 3) && lora_regq_want_wake(&q));
    lora_regq_clear(&q); // a C6 loss resets the receive queue and the token with it
    CHECK(lora_regq_want_wake(&q));
}

// The restart blank's pause after a recent REG_SET (#310 review M3)
static void test_settle(void) {
    CHECK(lora_regq_settle_ms(false, 0, 50) == 0);
    CHECK(lora_regq_settle_ms(true, 1000, 1000) == LORA_REGQ_SETTLE_MS);
    CHECK(lora_regq_settle_ms(true, 1000, 1000 + LORA_REGQ_SETTLE_RECENT_MS - 1) == LORA_REGQ_SETTLE_MS);
    CHECK(lora_regq_settle_ms(true, 1000, 1000 + LORA_REGQ_SETTLE_RECENT_MS) == 0);
    CHECK(lora_regq_settle_ms(true, UINT32_MAX - 10, 20) == LORA_REGQ_SETTLE_MS); // across the wrap
}

// The in-flight window (#314): at most LORA_REGQ_IN_FLIGHT unanswered, an answer frees its slot, a request is
// forgotten LORA_REGQ_ANSWER_MS after it left, and the task's queue wait ends when the oldest one is due
static void test_window(void) {
    lora_regq_t     q = {0};
    lora_regq_req_t r;
    for (uint16_t i = 0; i < 14; i++) { // a full `fw get`
        CHECK(post(&q, (uint16_t)(0x0300 + i), false, 0, (uint8_t)(10 + i)));
    }
    for (uint16_t i = 0; i < LORA_REGQ_IN_FLIGHT; i++) {
        CHECK(lora_regq_take(&q, 100, 0, 8, &r) && r.id == 0x0300 + i);
    }
    CHECK(!lora_regq_take(&q, 100, 0, 8, &r)); // the window is full
    CHECK(lora_regq_wait_ms(&q, 150, 5000) == LORA_REGQ_ANSWER_MS - 50);
    CHECK(!lora_regq_answered(&q, 0x0300, 99)); // a late answer to an older seq frees nothing
    CHECK(!lora_regq_answered(&q, 0x0700, 10)); // nor one nobody asked for
    CHECK(lora_regq_answered(&q, 0x0301, 11));
    CHECK(!lora_regq_answered(&q, 0x0301, 11)); // a duplicate
    CHECK(lora_regq_take(&q, 160, 0, 8, &r) && r.id == 0x0300 + LORA_REGQ_IN_FLIGHT);
    lora_regq_sent(&q, r.id, r.seq, 200); // the send took 40 ms: its timeout starts when it left
    CHECK(!lora_regq_take(&q, 200, 0, 8, &r));
    // A silent C6: the requests are forgotten, the queue moves on, never wedged
    CHECK(!lora_regq_take(&q, 100 + LORA_REGQ_ANSWER_MS - 1, 0, 8, &r));
    CHECK(lora_regq_wait_ms(&q, 100 + LORA_REGQ_ANSWER_MS, 5000) == 0);
    uint32_t t = 100 + LORA_REGQ_ANSWER_MS;
    CHECK(lora_regq_take(&q, t, 0, 8, &r) && r.id == 0x0305);
    CHECK(lora_regq_take(&q, t, 0, 8, &r) && r.id == 0x0306);
    CHECK(lora_regq_take(&q, t, 0, 8, &r) && r.id == 0x0307);
    CHECK(!lora_regq_take(&q, t, 0, 8, &r)); // the one that left at 200 still holds a slot
    CHECK(!lora_regq_take(&q, 200 + LORA_REGQ_ANSWER_MS - 1, 0, 8, &r));
    CHECK(q.timeouts == 3);
    CHECK(!lora_regq_answered(&q, 0x0300, 10)); // its answer after the timeout frees nothing more
    unsigned taken = 8, rounds = 0;
    for (t += LORA_REGQ_ANSWER_MS; taken < 14 && rounds < 10; t += LORA_REGQ_ANSWER_MS, rounds++) {
        while (lora_regq_take(&q, t, 0, 8, &r)) {
            taken++;
        }
    }
    CHECK(taken == 14 && q.count == 0);
    CHECK(lora_regq_wait_ms(&q, t, 5000) == 5000); // nothing waits
    lora_regq_clear(&q); // a C6 loss: nothing is in flight any more
    CHECK(q.n_fly == 0);
    // Wrap-around of the clock
    CHECK(post(&q, 0x0400, false, 0, 1) && lora_regq_take(&q, UINT32_MAX - 10, 0, 8, &r));
    CHECK(lora_regq_answered(&q, 0x0400, 1));
}

// The receive queue's room (#314 review M1): messages already waiting in it, every answer still to come, the new
// request's own and one wake token always leave LORA_REGQ_RX_ROOM places for RX_PACKET
static void test_rx_room(void) {
    lora_regq_t     q = {0};
    lora_regq_req_t r;
    for (uint16_t i = 0; i < 8; i++) {
        CHECK(post(&q, (uint16_t)(0x0500 + i), false, 0, (uint8_t)i));
    }
    for (size_t waiting = 0; waiting <= 8; waiting++) { // whatever sits in the queue, the room holds after each send
        lora_regq_t w = q;
        while (lora_regq_take(&w, 0, waiting, 8, &r)) {
        }
        CHECK(!w.n_fly || waiting + w.n_fly + 1 + LORA_REGQ_RX_ROOM <= 8);
        CHECK(w.n_fly == (waiting <= 3 ? 4 - waiting : 0));
    }
    // Received packets fill the queue: no request goes out until the task has read them
    CHECK(!lora_regq_take(&q, 0, 4, 8, &r));
    CHECK(lora_regq_take(&q, 0, 3, 8, &r));
    CHECK(!lora_regq_take(&q, 0, 3, 8, &r)); // its own answer now counts
    CHECK(lora_regq_take(&q, 0, 2, 8, &r));
    // A deeper queue still caps the window
    while (lora_regq_take(&q, 0, 0, 32, &r)) {
    }
    CHECK(q.n_fly == LORA_REGQ_IN_FLIGHT);
    // A forgotten request's place is free again
    CHECK(lora_regq_take(&q, LORA_REGQ_ANSWER_MS, 0, 8, &r) && q.n_fly == 1);
    CHECK(lora_regq_ahead(&q) == q.count + 1u);
}

// The console's live wait covers a burst paced through the window when every window but the last stays unanswered
static void test_burst(void) {
    CHECK(lora_regq_burst_ms(0) == 0);
    CHECK(lora_regq_burst_ms(LORA_REGQ_IN_FLIGHT) == 0);
    CHECK(lora_regq_burst_ms(LORA_REGQ_IN_FLIGHT + 1) == LORA_REGQ_ANSWER_MS);
    CHECK(lora_regq_burst_ms(14) == 3 * LORA_REGQ_ANSWER_MS);
    lora_regq_t     q = {0};
    lora_regq_req_t r;
    for (uint16_t i = 0; i < 6; i++) {
        CHECK(post(&q, (uint16_t)(0x0600 + i), false, 0, (uint8_t)i));
    }
    CHECK(lora_regq_take(&q, 0, 0, 8, &r) && lora_regq_take(&q, 0, 0, 8, &r));
    CHECK(lora_regq_ahead(&q) == 6); // waiting and in flight: what a new ask goes out behind
}

int main(void) {
    test_window();
    test_rx_room();
    test_burst();
    test_wake();
    test_settle();
    test_order_and_coalesce();
    test_full();
    test_block();
    if (failures) {
        printf("lora_regq_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("lora_regq_test: ok\n");
    return 0;
}
