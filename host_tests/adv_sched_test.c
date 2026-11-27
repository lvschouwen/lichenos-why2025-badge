// Host tests for the advertising schedule (#599): fast 30 s, slow, off; slow only for bonded peers; only while asked
// and while there is room; the connection parameters
#include "adv_sched.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static adv_wish_t const ASK = {.want = true, .room = true};

static void round_trip(void) {
    adv_sched_t s;
    adv_sched_init(&s);
    uint32_t t0 = 0xffff0000u;                                            // across the wrap
    CHECK(adv_sched_step(&s, t0, (adv_wish_t){.room = true}) == ADV_OFF); // nobody asks
    CHECK(adv_sched_step(&s, t0, ASK) == ADV_FAST);
    CHECK(adv_sched_interval_ms(ADV_FAST) == 100);
    CHECK(adv_sched_next_ms(&s, t0, ASK) == ADV_FAST_MS);
    CHECK(adv_sched_step(&s, t0 + ADV_FAST_MS - 1, ASK) == ADV_FAST);
    CHECK(adv_sched_step(&s, t0 + ADV_FAST_MS, ASK) == ADV_SLOW);
    CHECK(adv_sched_interval_ms(ADV_SLOW) == 1000);
    CHECK(adv_sched_next_ms(&s, t0 + ADV_FAST_MS, ASK) == ADV_SLOW_MS);
    CHECK(adv_sched_step(&s, t0 + ADV_FAST_MS + ADV_SLOW_MS - 1, ASK) == ADV_SLOW);
    CHECK(adv_sched_step(&s, t0 + ADV_FAST_MS + ADV_SLOW_MS, ASK) == ADV_OFF);
    CHECK(adv_sched_next_ms(&s, t0 + ADV_FAST_MS + ADV_SLOW_MS, ASK) == 0);
    CHECK(adv_sched_interval_ms(ADV_OFF) == 0);
    // still asked, round over: stays off until a restart
    CHECK(adv_sched_step(&s, t0 + 10000000, ASK) == ADV_OFF);
    adv_sched_restart(&s);
    CHECK(adv_sched_step(&s, t0 + 10000000, ASK) == ADV_FAST);
    // the ask ends: off at once; asked again: a new round
    CHECK(adv_sched_step(&s, t0 + 10000001, (adv_wish_t){.room = true}) == ADV_OFF);
    CHECK(adv_sched_next_ms(&s, t0 + 10000001, (adv_wish_t){.room = true}) == 0);
    CHECK(adv_sched_step(&s, t0 + 10050000, ASK) == ADV_FAST);
}

static void bonded_and_room(void) {
    adv_sched_t s;
    adv_sched_init(&s);
    adv_wish_t b = {.want = true, .bonded_only = true, .room = true};
    CHECK(adv_sched_step(&s, 0, b) == ADV_SLOW); // slow only
    CHECK(adv_sched_next_ms(&s, 0, b) == ADV_SLOW_MS);
    CHECK(adv_sched_step(&s, ADV_SLOW_MS - 1, b) == ADV_SLOW);
    CHECK(adv_sched_step(&s, ADV_SLOW_MS, b) == ADV_OFF);
    // a full badge does not advertise; the round's clock runs on meanwhile
    adv_sched_init(&s);
    adv_wish_t full = {.want = true, .room = false};
    CHECK(adv_sched_step(&s, 0, full) == ADV_OFF);
    CHECK(adv_sched_step(&s, 20000, ASK) == ADV_FAST);
    CHECK(adv_sched_step(&s, ADV_FAST_MS, ASK) == ADV_SLOW);
}

static void conn_params(void) {
    CHECK(BT_CONN_ITVL_MIN * 125u == 3000u && BT_CONN_ITVL_MAX * 125u == 5000u); // 30 and 50 ms, in 10 us
    CHECK(BT_CONN_LATENCY_MIN == 0 && BT_CONN_LATENCY_MAX == 4);
    CHECK(BT_CONN_SUPERVISION * 10u == 5000u);
    // the spec's rule: timeout > (1 + latency) * interval * 2, at the worst case (ms * 100 to stay in integers)
    CHECK(BT_CONN_SUPERVISION * 1000u > (1u + BT_CONN_LATENCY_MAX) * BT_CONN_ITVL_MAX * 125u * 2u);
}

int main(void) {
    round_trip();
    bonded_and_room();
    conn_params();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("adv_sched_test: all passed\n");
    return 0;
}
