// Host test: the outgoing message table and its delivery state machine (#45, #46, #47)
#include "mesh_outbox.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static mesh_out_state_t state_of(ob_t *ob, mesh_msg_id_t id) {
    mesh_out_state_t st = MESH_OUT_FAILED;
    CHECK(ob_get(ob, id, &st, NULL, NULL, NULL));
    return st;
}
static mf_msg_t reply(mf_kind_t kind) {
    mf_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = kind;
    return m;
}
static mf_msg_t sent(uint32_t ack, uint32_t est, uint8_t flood) {
    mf_msg_t m = reply(MF_SENT);
    m.ack = ack, m.est_timeout = est, m.flood = flood;
    return m;
}
static ob_action_t take(ob_t *ob, uint32_t now, ob_act_kind_t want) {
    ob_action_t a;
    CHECK(ob_next_action(ob, now, 1000, &a));
    CHECK(a.kind == want);
    return a;
}

static uint8_t const PEER[6] = {1, 2, 3, 4, 5, 6};

static void channel_flow(void) {
    ob_t ob;
    ob_init(&ob);
    char const  *why = NULL;
    mesh_msg_id_t id = ob_add_channel(&ob, "#test", "hello", 5, 0, &why);
    CHECK(id == 1 && state_of(&ob, id) == MESH_OUT_QUEUED);
    ob_action_t a = take(&ob, 0, OB_ACT_SEND_CHANNEL);
    CHECK(a.id == id && strcmp(a.target, "#test") == 0 && strcmp(a.text, "hello") == 0 && a.ts == 1000);
    ob_action_t none;
    CHECK(!ob_next_action(&ob, 0, 1000, &none)); // in flight: nothing else to do
    mf_msg_t ok = reply(MF_OK);
    ob_on_reply(&ob, id, &ok, 10);
    CHECK(state_of(&ob, id) == MESH_OUT_SENT);
    uint8_t h[OB_HASH_LEN] = {1, 2, 3, 4, 5, 6, 7, 8};
    ob_on_tx(&ob, false, h);
    CHECK(state_of(&ob, id) == MESH_OUT_ON_AIR);
    ob_on_rx(&ob, h);
    ob_on_rx(&ob, h);
    uint8_t other[OB_HASH_LEN] = {9};
    ob_on_rx(&ob, other);
    uint8_t reps = 0;
    mesh_out_state_t st;
    CHECK(ob_get(&ob, id, &st, NULL, &reps, NULL) && st == MESH_OUT_HEARD && reps == 2);
}

static void channel_failures(void) {
    ob_t ob;
    ob_init(&ob);
    char const *why = NULL;
    mesh_msg_id_t id = ob_add_channel(&ob, "#test", "x", 1, 0, &why);
    take(&ob, 0, OB_ACT_SEND_CHANNEL);
    mf_msg_t err = reply(MF_ERR);
    err.err      = MF_ERR_NOT_FOUND;
    ob_on_reply(&ob, id, &err, 5);
    CHECK(state_of(&ob, id) == MESH_OUT_FAILED);
    // the radio refusing a packet fails the channel message
    mesh_msg_id_t id2 = ob_add_channel(&ob, "#test", "y", 1, 0, &why);
    take(&ob, 0, OB_ACT_SEND_CHANNEL);
    mf_msg_t ok = reply(MF_OK);
    ob_on_reply(&ob, id2, &ok, 5);
    ob_on_tx_fail(&ob, false, 6);
    CHECK(state_of(&ob, id2) == MESH_OUT_FAILED);
    // a failed one can be retried under the same id, with a fresh timestamp
    CHECK(ob_retry(&ob, id2, 20));
    ob_action_t a;
    CHECK(ob_next_action(&ob, 20, 2000, &a) && a.id == id2 && a.ts == 2000);
    CHECK(!ob_retry(&ob, id2, 30)); // not failed any more
    CHECK(ob_add_channel(&ob, "#t", "", 0, 0, &why) == 0 && strcmp(why, "empty") == 0);
    CHECK(ob_add_channel(&ob, "#t", "x", 161, 0, &why) == 0);
}

static void dm_delivered(void) {
    ob_t ob;
    ob_init(&ob);
    char const *why = NULL;
    mesh_msg_id_t id = ob_add_dm(&ob, PEER, "hi", 2, 0, &why);
    ob_action_t   a  = take(&ob, 0, OB_ACT_SEND_DM);
    CHECK(a.attempt == 0 && memcmp(a.prefix, PEER, 6) == 0);
    mf_msg_t s = sent(0xAA, 3000, 0);
    ob_on_reply(&ob, id, &s, 100);
    CHECK(state_of(&ob, id) == MESH_OUT_SENT);
    uint8_t h[OB_HASH_LEN] = {1};
    ob_on_tx(&ob, true, h);
    CHECK(state_of(&ob, id) == MESH_OUT_ON_AIR);
    ob_on_confirm(&ob, 0xAA, 1234);
    uint32_t trip = 0;
    CHECK(ob_get(&ob, id, NULL, NULL, NULL, &trip) && trip == 1234 && state_of(&ob, id) == MESH_OUT_DELIVERED);
    ob_on_confirm(&ob, 0xAA, 9999); // duplicate ACK
    CHECK(ob_get(&ob, id, NULL, NULL, NULL, &trip) && trip == 1234);
}

// Timeout 3 s * 1.25 = 3750 ms: three tries, the reset-path before the last, FAILED after
static void dm_retries(void) {
    ob_t ob;
    ob_init(&ob);
    char const *why = NULL;
    mesh_msg_id_t id = ob_add_dm(&ob, PEER, "hi", 2, 0, &why);
    mf_msg_t      ok = reply(MF_OK);
    (void)ok;
    ob_action_t a = take(&ob, 0, OB_ACT_SEND_DM);
    uint32_t ts   = a.ts;
    mf_msg_t s1   = sent(0x01, 3000, 0);
    ob_on_reply(&ob, id, &s1, 0);
    ob_action_t none;
    CHECK(!ob_next_action(&ob, 3749, 5000, &none)); // still waiting
    a = take(&ob, 3750, OB_ACT_SEND_DM);
    CHECK(a.attempt == 1 && a.ts == ts); // same timestamp and text
    mf_msg_t s2 = sent(0x02, 3000, 0);
    ob_on_reply(&ob, id, &s2, 3750);
    // second direct try failed: forget the path, then the last try
    a = take(&ob, 7500, OB_ACT_RESET_PATH);
    CHECK(a.id == id);
    ob_on_reply(&ob, id, &(mf_msg_t){.kind = MF_OK}, 7500);
    a = take(&ob, 7500, OB_ACT_SEND_DM);
    CHECK(a.attempt == 2 && a.ts == ts);
    mf_msg_t s3 = sent(0x03, 3000, 1); // flooded
    ob_on_reply(&ob, id, &s3, 7500);
    CHECK(!ob_next_action(&ob, 11249, 5000, &none));
    CHECK(!ob_next_action(&ob, 11250, 5000, &none) && state_of(&ob, id) == MESH_OUT_FAILED);
    // the ACK of the first attempt still arrives: delivered after all
    ob_on_confirm(&ob, 0x01, 77);
    CHECK(state_of(&ob, id) == MESH_OUT_DELIVERED);
}

static void dm_retry_and_errors(void) {
    ob_t ob;
    ob_init(&ob);
    char const *why = NULL;
    mesh_msg_id_t id = ob_add_dm(&ob, PEER, "hi", 2, 0, &why);
    take(&ob, 0, OB_ACT_SEND_DM);
    mf_msg_t full = reply(MF_ERR);
    full.err      = MF_ERR_TABLE_FULL;
    ob_on_reply(&ob, id, &full, 0);
    ob_action_t none;
    CHECK(state_of(&ob, id) == MESH_OUT_QUEUED && !ob_next_action(&ob, 1999, 1, &none)); // backs off
    take(&ob, 2000, OB_ACT_SEND_DM);
    ob_on_reply(&ob, id, &full, 2000);
    take(&ob, 4000, OB_ACT_SEND_DM);
    ob_on_reply(&ob, id, &full, 4000);
    CHECK(state_of(&ob, id) == MESH_OUT_FAILED); // three attempts used
    uint8_t att = 0;
    CHECK(ob_get(&ob, id, NULL, &att, NULL, NULL) && att == 3);
    CHECK(ob_retry(&ob, id, 5000));
    CHECK(ob_get(&ob, id, NULL, &att, NULL, NULL) && att == 0);
    ob_action_t a = take(&ob, 5000, OB_ACT_SEND_DM);
    CHECK(a.attempt == 0);
    mf_msg_t nf = reply(MF_ERR);
    nf.err      = MF_ERR_NOT_FOUND;
    ob_on_reply(&ob, id, &nf, 5000);
    CHECK(state_of(&ob, id) == MESH_OUT_FAILED);
    // a radio failure ends the wait for the ACK early
    mesh_msg_id_t id2 = ob_add_dm(&ob, PEER, "again", 5, 6000, &why);
    take(&ob, 6000, OB_ACT_SEND_DM);
    mf_msg_t s = sent(0x10, 30000, 0);
    ob_on_reply(&ob, id2, &s, 6000);
    ob_on_tx_fail(&ob, true, 6100);
    ob_action_t early;
    CHECK(!ob_next_action(&ob, 6100, 1000, &early)); // a pause first: three refusals must not burn all attempts at once
    take(&ob, 6100 + OB_RETRY_BACKOFF_MS, OB_ACT_SEND_DM);
    CHECK(id2 != 0);
}

static void limits(void) {
    ob_t ob;
    ob_init(&ob);
    char const *why = NULL;
    uint8_t     p[6] = {0};
    CHECK(ob_add_dm(&ob, PEER, "a", 1, 0, &why) != 0);
    CHECK(ob_add_dm(&ob, PEER, "b", 1, 0, &why) == 0 && strcmp(why, "busy") == 0); // one per contact
    for (uint8_t i = 1; i < OB_MAX_DM_INFLIGHT; i++) {
        p[0] = i;
        CHECK(ob_add_dm(&ob, p, "c", 1, 0, &why) != 0);
    }
    p[0] = 99;
    CHECK(ob_add_dm(&ob, p, "d", 1, 0, &why) == 0); // limit in flight
    CHECK((OB_MAX_DM_INFLIGHT + 1) * OB_MAX_ATTEMPTS < OB_ACK_SLOTS + OB_MAX_ATTEMPTS); // the ACK table fits them
    CHECK(OB_MAX_DM_INFLIGHT * OB_MAX_ATTEMPTS < OB_ACK_SLOTS);
    // the table fills with channel messages, then refuses; sent ones make room
    ob_t full;
    ob_init(&full);
    for (int i = 0; i < OB_CAP; i++) {
        CHECK(ob_add_channel(&full, "#t", "x", 1, 0, &why) != 0);
    }
    CHECK(ob_add_channel(&full, "#t", "x", 1, 0, &why) == 0 && strcmp(why, "busy") == 0);
    ob_action_t a = take(&full, 0, OB_ACT_SEND_CHANNEL);
    mf_msg_t    ok = reply(MF_OK);
    ob_on_reply(&full, a.id, &ok, 1);
    CHECK(ob_add_channel(&full, "#t", "x", 1, 0, &why) != 0);
}

// Every in-flight DM can register one ACK per attempt: their total must fit MyMesh's table (8)
static void ack_slots(void) {
    ob_t ob;
    ob_init(&ob);
    char const *why = NULL;
    int         added = 0;
    for (uint8_t i = 1; i <= 6; i++) {
        uint8_t p[6] = {i};
        added += ob_add_dm(&ob, p, "x", 1, 0, &why) != 0;
    }
    CHECK(added * OB_MAX_ATTEMPTS < OB_ACK_SLOTS);
    CHECK(added == 2);
}

// A drain keeps refilling the internal FIFO: the outbox still gets every second frame
static void arbitration(void) {
    ob_arb_t arb = {0};
    int      internal = 0, outbox = 0;
    for (int i = 0; i < 10; i++) {
        if (ob_arb_outbox_first(&arb, true)) {
            ob_arb_took(&arb, false), outbox++;
        } else {
            ob_arb_took(&arb, true), internal++;
        }
    }
    CHECK(internal == 5 && outbox == 5);
    ob_arb_t idle = {1};
    CHECK(ob_arb_outbox_first(&idle, false)); // nothing queued internally: outbox as usual
}

static void wrap(void) {
    ob_t ob;
    ob_init(&ob);
    ob.next_id = 0xFFFFFFFF;
    char const *why = NULL;
    mesh_msg_id_t id = ob_add_channel(&ob, "#t", "x", 1, 0, &why);
    CHECK(id == 1); // 0 is skipped
    // millis wrap: a deadline just before the wrap is due just after it
    ob_t w;
    ob_init(&w);
    mesh_msg_id_t d = ob_add_dm(&w, PEER, "x", 1, 0xFFFFFC00u, &why);
    ob_action_t a;
    CHECK(ob_next_action(&w, 0xFFFFFC00u, 1, &a));
    mf_msg_t s = sent(5, 3000, 0);
    ob_on_reply(&w, d, &s, 0xFFFFFC00u); // deadline = 0xFFFFF000 + 3750 wraps past 0
    CHECK(!ob_next_action(&w, 0xFFFFFFFFu, 1, &a));
    CHECK(ob_next_action(&w, 0xFFFFFC00u + 3750u, 1, &a) && a.kind == OB_ACT_SEND_DM);
}

int main(void) {
    channel_flow();
    channel_failures();
    dm_delivered();
    dm_retries();
    dm_retry_and_errors();
    limits();
    ack_slots();
    arbitration();
    wrap();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_outbox_test: all checks passed\n");
    return 0;
}
