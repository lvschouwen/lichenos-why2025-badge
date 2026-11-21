// Host test for the Bluetooth switch -> C6 bt.enable decisions (#503)
#include "bt_sync.h"
#include "reg.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static bt_sync_in_t base(bool want) {
    return (bt_sync_in_t){.want = want, .support = RP_OK};
}

static void old_c6(void) {
    // No registry, no Bluetooth domain, or UNKNOWN for the key: never a SET, whatever is wanted
    for (int w = 0; w < 2; w++) {
        bt_sync_in_t in = base(w);
        in.support      = RP_OLD_C6;
        CHECK(bt_sync_decide(&in) == BT_SYNC_UNSUPPORTED);
        in.support = RP_NO_DOMAIN;
        CHECK(bt_sync_decide(&in) == BT_SYNC_UNSUPPORTED);
        in         = base(w);
        in.unknown_id = true;
        CHECK(bt_sync_decide(&in) == BT_SYNC_UNSUPPORTED);
        in.tries = 3; // still unsupported, not a give-up: the Settings row says why
        CHECK(bt_sync_decide(&in) == BT_SYNC_UNSUPPORTED);
    }
}

static void mapping(void) {
    bt_sync_in_t in = base(true);
    in.support      = RP_WAIT_CAPS;
    CHECK(bt_sync_decide(&in) == BT_SYNC_WAIT); // boot: caps not answered yet
    in.support = RP_OK;
    CHECK(bt_sync_decide(&in) == BT_SYNC_SEND); // switch on, nothing known: ask the C6 to enable
    in.applied_known = true;
    in.applied_on    = false;
    CHECK(bt_sync_decide(&in) == BT_SYNC_SEND); // known off: enable
    in.applied_on = true;
    CHECK(bt_sync_decide(&in) == BT_SYNC_IDLE); // the readback says enabled: done

    in      = base(false); // airplane on (or switch off): the wish is off
    CHECK(bt_sync_decide(&in) == BT_SYNC_SEND); // nothing known after a boot: one SET off, to read it back
    in.applied_known = true;
    in.applied_on    = true;
    CHECK(bt_sync_decide(&in) == BT_SYNC_SEND);
    in.applied_on = false;
    CHECK(bt_sync_decide(&in) == BT_SYNC_IDLE);
}

static void no_storm(void) {
    bt_sync_in_t in = base(true);
    in.applied_known = true;
    in.tries         = 1;
    in.since_sent_ms = BT_SYNC_RETRY_MS - 1;
    CHECK(bt_sync_decide(&in) == BT_SYNC_WAIT); // just sent
    in.since_sent_ms = BT_SYNC_RETRY_MS;
    CHECK(bt_sync_decide(&in) == BT_SYNC_SEND); // no result in time: once more
    in.tries = BT_SYNC_TRIES;
    CHECK(bt_sync_decide(&in) == BT_SYNC_GIVE_UP); // then no more, until a new wish or link resets tries
    in.applied_on = true;
    CHECK(bt_sync_decide(&in) == BT_SYNC_IDLE); // a late answer that matches ends it
}

// bt_sync_t with a real proxy cache (reg_proxy.c): what lichen_reg.c does around bt_sync_decide
static void bookkeeping(void) {
    uint16_t const ids[]   = {REG_ID_C6_REG_CAPS, REG_ID_BT_ENABLE};
    uint8_t const  types[] = {REG_T_U32, REG_T_BOOL};
    reg_proxy_t    p;
    CHECK(reg_proxy_init(&p, ids, types, 2, 0));
    bt_sync_t s = {0};
    uint8_t   seq;

    CHECK(bt_sync_step(&s, reg_proxy_slot(&p, REG_ID_BT_ENABLE), RP_OK, 100) == BT_SYNC_IDLE); // no wish told yet
    bt_sync_wish(&s, true);
    CHECK(bt_sync_step(&s, reg_proxy_slot(&p, REG_ID_BT_ENABLE), RP_OK, 100) == BT_SYNC_SEND);
    bt_sync_sent(&s, 100);
    CHECK(bt_sync_in_flight(&s, 4000) && !bt_sync_in_flight(&s, 100 + BT_SYNC_RETRY_MS));
    CHECK(bt_sync_step(&s, reg_proxy_slot(&p, REG_ID_BT_ENABLE), RP_OK, 4000) == BT_SYNC_WAIT);
    // the answer: enabled
    CHECK(reg_proxy_begin(&p, REG_ID_BT_ENABLE, 100, &seq));
    CHECK(reg_proxy_answer(&p, REG_ID_BT_ENABLE, seq, REG_OK, REG_T_BOOL, 1, 200) == RP_ACCEPTED);
    CHECK(bt_sync_step(&s, reg_proxy_slot(&p, REG_ID_BT_ENABLE), RP_OK, 300) == BT_SYNC_IDLE);

    // a changed wish starts the count again, the same wish does not
    bt_sync_wish(&s, true);
    CHECK(s.tries == 1);
    bt_sync_wish(&s, false);
    CHECK(s.tries == 0 && !s.said);
    CHECK(bt_sync_step(&s, reg_proxy_slot(&p, REG_ID_BT_ENABLE), RP_OK, 6000) == BT_SYNC_SEND);

    // three tries, then give up; a link-up starts over and the stale answer no longer counts as applied
    for (int i = 0; i < BT_SYNC_TRIES; i++) {
        bt_sync_sent(&s, 6000 + i * BT_SYNC_RETRY_MS);
    }
    CHECK(bt_sync_say(&s) && !bt_sync_say(&s)); // the line is written once
    CHECK(bt_sync_step(&s, reg_proxy_slot(&p, REG_ID_BT_ENABLE), RP_OK, 6000 + 10 * BT_SYNC_RETRY_MS) == BT_SYNC_GIVE_UP);
    reg_proxy_link(&p, 90000);
    bt_sync_link(&s);
    CHECK(s.tries == 0 && !s.said);
    CHECK(bt_sync_step(&s, reg_proxy_slot(&p, REG_ID_BT_ENABLE), RP_OK, 90000) == BT_SYNC_SEND); // stale: asked again

    // UNKNOWN from a C6 without the key: unsupported, until a relink makes the answer stale (the C6 may be updated)
    CHECK(reg_proxy_begin(&p, REG_ID_BT_ENABLE, 91000, &seq));
    CHECK(reg_proxy_answer(&p, REG_ID_BT_ENABLE, seq, REG_UNKNOWN, 0, 0, 91100) == RP_ACCEPTED);
    CHECK(bt_sync_step(&s, reg_proxy_slot(&p, REG_ID_BT_ENABLE), RP_OK, 92000) == BT_SYNC_UNSUPPORTED);
    reg_proxy_link(&p, 95000);
    bt_sync_link(&s);
    CHECK(bt_sync_step(&s, reg_proxy_slot(&p, REG_ID_BT_ENABLE), RP_OK, 95001) == BT_SYNC_SEND);
    // the input of a missing slot is "nothing known"
    CHECK(bt_sync_step(&s, NULL, RP_OK, 95001) == BT_SYNC_SEND);
}

int main(void) {
    bookkeeping();
    old_c6();
    mapping();
    no_storm();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("bt_sync_test ok\n");
    return 0;
}
