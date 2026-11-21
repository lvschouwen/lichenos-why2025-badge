// Host test for the Bluetooth switch -> C6 bt.enable decisions (#503)
#include "bt_sync.h"

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

int main(void) {
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
