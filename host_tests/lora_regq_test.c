// Host tests for the LoRa task's registry request queue (#310): order, one entry per id (a newer SET wins, a GET keeps
// a waiting SET), the newest seq, a full queue, and the restart block (no backlight REG_SET after the UART blank)
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

static void test_order_and_coalesce(void) {
    lora_regq_t     q = {0};
    lora_regq_req_t r;
    lora_regq_clear(&q);
    CHECK(!lora_regq_take(&q, &r));
    CHECK(post(&q, 0x0100, true, 40, 1));
    CHECK(post(&q, 0x0101, true, 10, 2));
    CHECK(post(&q, 0x0100, true, 45, 3)); // a slider step: replaces, keeps its place
    CHECK(post(&q, 0x0100, false, 0, 4)); // a GET after it: the SET stays, with the newer seq
    CHECK(post(&q, 0x0200, false, 0, 5));
    CHECK(q.count == 3);
    CHECK(lora_regq_take(&q, &r) && r.id == 0x0100 && r.set && r.value == 45 && r.seq == 4);
    CHECK(lora_regq_take(&q, &r) && r.id == 0x0101 && r.set && r.value == 10 && r.seq == 2);
    CHECK(post(&q, 0x0200, true, 7, 6)); // a SET over a waiting GET turns it into the SET
    CHECK(lora_regq_take(&q, &r) && r.id == 0x0200 && r.set && r.value == 7 && r.seq == 6);
    CHECK(!lora_regq_take(&q, &r));
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
        CHECK(lora_regq_take(&q, &r) && r.id == i);
    }
    CHECK(post(&q, 0x0F00, false, 0, 101)); // wraps around
    for (uint16_t i = 5; i < LORA_REGQ_MAX; i++) {
        CHECK(lora_regq_take(&q, &r) && r.id == i);
    }
    CHECK(lora_regq_take(&q, &r) && r.id == 0x0F00 && r.seq == 101);
    CHECK(!lora_regq_take(&q, &r));
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
    CHECK(lora_regq_take(&q, &r) && r.id == 0x0200 && r.seq == 8);
    CHECK(!lora_regq_take(&q, &r));
    // Order of the others across a wrapped ring
    lora_regq_t w = {0};
    for (uint16_t i = 0; i < LORA_REGQ_MAX; i++) {
        CHECK(post(&w, (uint16_t)(0x300 + i), false, 0, (uint8_t)i));
    }
    for (int i = 0; i < 10; i++) {
        CHECK(lora_regq_take(&w, &r));
    }
    for (uint16_t i = 0; i < 6; i++) {
        CHECK(post(&w, (uint16_t)(0x400 + i), false, 0, 0));
    }
    CHECK(lora_regq_block(&w, 0x30C));
    CHECK(lora_regq_block(&w, 0x401));
    uint16_t const want[] = {0x30A, 0x30B, 0x30D, 0x30E, 0x30F, 0x400, 0x402, 0x403, 0x404, 0x405};
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        CHECK(lora_regq_take(&w, &r) && r.id == want[i]);
    }
    CHECK(!lora_regq_take(&w, &r));
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
    while (lora_regq_take(&q, &r)) {
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

int main(void) {
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
