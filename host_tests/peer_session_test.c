// Host tests for the badge-to-badge session (#599): frames and invites (fuzzed: a remote surface), visible, invite,
// accept and decline, pairing, the ordered channel (window, acks, retransmission, wrap), keepalive and silence, end,
// lost link and glare against the connection cap
#include "ble_policy.h"
#include "mutate.h"
#include "peer_session.h"

#include <stdio.h>
#include <stdlib.h>

#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// ---- Frames and invites ----

static peer_invite_t invite_of(uint32_t nonce, char const *app, char const *name) {
    peer_invite_t inv = {.nonce = nonce};
    strncpy(inv.app, app, sizeof(inv.app) - 1);
    strncpy(inv.name, name, sizeof(inv.name) - 1);
    return inv;
}

static void frames(void) {
    uint8_t      b[PEER_FRAME_MAX + 4], data[PEER_MSG_MAX + 1];
    peer_frame_t f;
    memset(data, 0x5a, sizeof(data));
    size_t n = peer_frame_build(PEER_F_DATA, 0x1234, 0xbeef, data, PEER_MSG_MAX, b, sizeof(b));
    CHECK(n == PEER_FRAME_MAX);
    CHECK(peer_frame_parse(b, n, &f) && f.type == PEER_F_DATA && f.seq == 0x1234 && f.ack == 0xbeef);
    CHECK(f.len == PEER_MSG_MAX && f.payload == b + PEER_HDR);
    CHECK(peer_frame_build(PEER_F_DATA, 0, 0, data, PEER_MSG_MAX + 1, b, sizeof(b)) == 0);
    CHECK(peer_frame_build(PEER_F_DATA, 0, 0, data, 0, b, sizeof(b)) == 0);
    CHECK(peer_frame_build(PEER_F_DATA, 0, 0, data, 4, b, PEER_HDR + 3) == 0); // cap
    CHECK(peer_frame_build(PEER_F_ACK, 0, 7, NULL, 0, b, PEER_HDR) == PEER_HDR);
    CHECK(peer_frame_build(PEER_F_ACK, 0, 7, data, 1, b, sizeof(b)) == 0);            // no payload on an ACK
    CHECK(peer_frame_build(PEER_F_END, 0, 7, data, 1, b, sizeof(b)) == PEER_HDR + 1); // END's reason byte
    CHECK(peer_frame_build(PEER_F_END, 0, 7, data, 2, b, sizeof(b)) == 0);
    CHECK(peer_frame_build(PEER_F_INVITE, 0, 0, data, PEER_INVITE_MAX + 1, b, sizeof(b)) == 0);
    CHECK(peer_frame_build((peer_ftype_t)9, 0, 0, NULL, 0, b, sizeof(b)) == 0);
    // parse: short, version, type, length lies
    n = peer_frame_build(PEER_F_ACK, 0, 7, NULL, 0, b, sizeof(b));
    CHECK(!peer_frame_parse(b, PEER_HDR - 1, &f));
    b[0] = 2;
    CHECK(!peer_frame_parse(b, n, &f));
    b[0] = PEER_VERSION;
    b[1] = 0;
    CHECK(!peer_frame_parse(b, n, &f));
    b[1] = 7;
    CHECK(!peer_frame_parse(b, n, &f));
    b[1] = PEER_F_ACK;
    CHECK(peer_frame_parse(b, n, &f));
    CHECK(!peer_frame_parse(b, n + 1, &f)); // a trailing byte the length does not cover
    n    = peer_frame_build(PEER_F_DATA, 0, 0, data, 10, b, sizeof(b));
    b[6] = 11;
    CHECK(!peer_frame_parse(b, n, &f));
    b[6] = 9;
    CHECK(!peer_frame_parse(b, n, &f));
}

static void invites(void) {
    uint8_t       b[PEER_INVITE_MAX + 8];
    peer_invite_t in = invite_of(0xa1b2c3d4u, "battleship", "Badge of Lucas"), out;
    in.app_version   = 0x0203;
    in.param_len     = 2;
    in.param[0]      = 10;
    in.param[1]      = 3;
    size_t n         = peer_invite_encode(&in, b, sizeof(b));
    CHECK(n == 4 + 1 + 10 + 2 + 1 + 14 + 1 + 2);
    CHECK(peer_invite_decode(b, n, &out));
    CHECK(out.nonce == in.nonce && strcmp(out.app, "battleship") == 0 && strcmp(out.name, in.name) == 0);
    CHECK(out.param_len == 2 && out.param[1] == 3 && out.app_version == 0x0203);
    // the largest: nonce 4, app 1 + 13, version 2, name 1 + 28, param 1 + 14 = 64 exactly
    peer_invite_t big = invite_of(1, "abcdefghijklm", "0123456789012345678901234567");
    big.param_len     = PEER_PARAM_MAX;
    CHECK(peer_invite_encode(&big, b, sizeof(b)) == PEER_INVITE_MAX);
    CHECK(peer_invite_decode(b, PEER_INVITE_MAX, &out) && out.param_len == PEER_PARAM_MAX);
    CHECK(peer_invite_encode(&big, b, PEER_INVITE_MAX - 1) == 0); // cap
    big.param_len = PEER_PARAM_MAX + 1;
    CHECK(peer_invite_encode(&big, b, sizeof(b)) == 0);
    // bad app ids and names
    peer_invite_t bad = invite_of(1, "", "x");
    CHECK(peer_invite_encode(&bad, b, sizeof(b)) == 0);
    bad = invite_of(1, "Battle", "x");
    CHECK(peer_invite_encode(&bad, b, sizeof(b)) == 0);
    bad = invite_of(1, "ok", "tab\there");
    CHECK(peer_invite_encode(&bad, b, sizeof(b)) == 0);
    bad = invite_of(1, "ok", ""); // no name is allowed
    n   = peer_invite_encode(&bad, b, sizeof(b));
    CHECK(n == 4 + 1 + 2 + 2 + 1 + 0 + 1 && peer_invite_decode(b, n, &out) && out.name[0] == '\0');
    // decode: a length past the end, a trailing byte, a control byte, an app too long
    n = peer_invite_encode(&in, b, sizeof(b));
    CHECK(!peer_invite_decode(b, n - 1, &out));
    b[n] = 0;
    CHECK(!peer_invite_decode(b, n + 1, &out));
    b[4] = 14;
    CHECK(!peer_invite_decode(b, n, &out));
    n     = peer_invite_encode(&in, b, sizeof(b));
    b[20] = 0x07; // inside the name
    CHECK(!peer_invite_decode(b, n, &out));
    CHECK(!peer_invite_decode(b, 3, &out));
    CHECK(!peer_invite_decode(b, 6, &out)); // the app id, then no room for the version
}

// Malformed frames and invites never crash, never read past the length (mut_exact + ASan), and never parse into
// something out of bounds
static void fuzz(void) {
    uint8_t       seed_inv[PEER_INVITE_MAX], seed[PEER_FRAME_MAX], buf[PEER_FRAME_MAX + 16];
    peer_invite_t in = invite_of(77, "battleship", "Peer");
    in.param_len     = 3;
    size_t il        = peer_invite_encode(&in, seed_inv, sizeof(seed_inv));
    size_t fl        = peer_frame_build(PEER_F_INVITE, 0, 0, seed_inv, il, seed, sizeof(seed));
    for (uint32_t i = 0; i < 20000; i++) {
        mut_rng_t r;
        mut_seed(&r, (i + 1) * 2654435761u);
        size_t len = mut_random(&r, seed, fl, buf, sizeof(buf));
        if (i % 5 == 0) {
            mut_lie_le(buf, len, 6, 2, mut_next(&r) & 0x3ff);
        }
        uint8_t     *x = mut_exact(buf, len);
        peer_frame_t f;
        if (peer_frame_parse(x, len, &f)) {
            CHECK(f.len + PEER_HDR == len && f.len <= PEER_MSG_MAX);
            peer_invite_t out;
            if (f.type == PEER_F_INVITE && peer_invite_decode(f.payload, f.len, &out)) {
                CHECK(strlen(out.app) >= 1 && strlen(out.app) <= PEER_APP_MAX);
                CHECK(strlen(out.name) <= PEER_NAME_MAX && out.param_len <= PEER_PARAM_MAX);
            }
        }
        // the session takes them too (Visible, idle): nothing but a verdict
        peer_session_t *s = calloc(1, sizeof(*s));
        if (!s || !x) {
            abort();
        }
        peer_session_set_visible(s, 0, true);
        peer_rx_t v = peer_session_on_frame(s, 1, 3, 3, x, len);
        CHECK(v == PEER_RX_OK || v == PEER_RX_BAD || v == PEER_RX_IGNORED);
        CHECK(s->state == PEER_IDLE || s->state == PEER_INVITED);
        free(s);
        free(x);
    }
}

// ---- Two badges on a wire ----

typedef struct {
    peer_session_t *s;
    uint16_t        link; // the session link's handle on this badge
    bool            drop; // frames from this badge are lost
    int             sent; // frames that left this badge
} badge_t;

// Moves frames both ways until both are quiet (at most a few hundred)
static void pump(badge_t *a, badge_t *b, uint32_t now) {
    uint8_t buf[PEER_FRAME_MAX];
    for (int guard = 0; guard < 400; guard++) {
        size_t na = peer_session_next_frame(a->s, now, buf, sizeof(buf));
        if (na) {
            a->sent++;
            if (!a->drop) {
                peer_session_on_frame(b->s, now, b->link, b->link, buf, na);
            }
        }
        size_t nb = peer_session_next_frame(b->s, now, buf, sizeof(buf));
        if (nb) {
            b->sent++;
            if (!b->drop) {
                peer_session_on_frame(a->s, now, a->link, a->link, buf, nb);
            }
        }
        if (!na && !nb) {
            return;
        }
    }
    printf("FAIL pump never settled\n");
    failures++;
}

static peer_session_t *new_session(void) {
    peer_session_t *s = malloc(sizeof(*s));
    if (!s) {
        abort(); // a host test without memory cannot go on
    }
    peer_session_init(s);
    return s;
}

// A invites B (B Visible), B accepts, both pair: open at time t
static void open_pair(badge_t *a, badge_t *b, uint32_t t) {
    peer_session_set_visible(b->s, t, true);
    peer_invite_t inv = invite_of(1, "battleship", "A");
    CHECK(peer_session_invite(a->s, t, a->link, &inv) == LICHEN_BT_OK);
    pump(a, b, t);
    CHECK(b->s->state == PEER_INVITED && (peer_session_take_events(b->s) & PEER_EV_INVITE));
    CHECK(peer_session_answer(b->s, t, true));
    pump(a, b, t);
    CHECK(a->s->state == PEER_PAIRING && (peer_session_take_events(a->s) & PEER_EV_PAIR));
    CHECK(b->s->state == PEER_PAIRING);
    peer_session_secured(a->s, t, true);
    peer_session_secured(b->s, t, true);
    CHECK(a->s->state == PEER_OPEN && b->s->state == PEER_OPEN);
    CHECK(peer_session_take_events(a->s) & PEER_EV_OPEN);
    CHECK(peer_session_take_events(b->s) & PEER_EV_OPEN);
}

static void happy_path(void) {
    badge_t a = {.s = new_session(), .link = 1}, b = {.s = new_session(), .link = 11};
    open_pair(&a, &b, 100);
    CHECK(a.s->role == LICHEN_BT_ROLE_CENTRAL && b.s->role == LICHEN_BT_ROLE_PERIPHERAL); // the inviter is central
    CHECK(strcmp(b.s->invite.app, "battleship") == 0 && strcmp(b.s->invite.name, "A") == 0);
    char msg[8];
    for (int i = 0; i < 3; i++) {
        snprintf(msg, sizeof(msg), "a%d", i);
        CHECK(peer_session_send(a.s, msg, strlen(msg) + 1) == LICHEN_BT_OK);
    }
    CHECK(peer_session_send(b.s, "b0", 3) == LICHEN_BT_OK);
    pump(&a, &b, 200);
    CHECK(peer_session_take_events(b.s) & PEER_EV_MSG);
    char got[PEER_MSG_MAX];
    for (int i = 0; i < 3; i++) {
        snprintf(msg, sizeof(msg), "a%d", i);
        CHECK(peer_session_recv(b.s, got, sizeof(got)) == 3 && strcmp(got, msg) == 0);
    }
    CHECK(peer_session_recv(b.s, got, sizeof(got)) == -1);
    CHECK(peer_session_recv(a.s, got, 2) == -1); // cap too small: it stays
    CHECK(peer_session_recv(a.s, got, sizeof(got)) == 3 && strcmp(got, "b0") == 0);
    CHECK(a.s->txq_n == 0 && b.s->txq_n == 0); // all acked
    // sizes
    CHECK(peer_session_send(a.s, got, 0) == LICHEN_BT_INVALID);
    CHECK(peer_session_send(a.s, got, PEER_MSG_MAX + 1) == LICHEN_BT_INVALID);
    CHECK(peer_session_send(a.s, got, PEER_MSG_MAX) == LICHEN_BT_OK);
    pump(&a, &b, 300);
    CHECK(peer_session_recv(b.s, got, sizeof(got)) == (int)PEER_MSG_MAX);
    // end
    peer_session_end(a.s, 400);
    CHECK(a.s->state == PEER_CLOSED && a.s->end == PEER_END_LOCAL);
    pump(&a, &b, 400);
    CHECK(b.s->state == PEER_CLOSED && b.s->end == PEER_END_REMOTE);
    CHECK(peer_session_take_events(b.s) & PEER_EV_CLOSED);
    CHECK(peer_session_send(a.s, "x", 1) == LICHEN_BT_NOT_CONNECTED);
    free(a.s);
    free(b.s);
}

static void app_exit(void) {
    badge_t a = {.s = new_session(), .link = 1}, b = {.s = new_session(), .link = 11};
    open_pair(&a, &b, 0);
    peer_session_app_exit(a.s, 10);
    CHECK(a.s->state == PEER_CLOSED && a.s->end == PEER_END_APP_EXIT);
    uint8_t f[PEER_FRAME_MAX];
    size_t  n = peer_session_next_frame(a.s, 10, f, sizeof(f));
    CHECK(n == PEER_HDR + 1 && f[1] == PEER_F_END && f[PEER_HDR] == PEER_WHY_APP_EXIT);
    CHECK(peer_session_on_frame(b.s, 10, 11, 11, f, n) == PEER_RX_OK);
    CHECK(b.s->state == PEER_CLOSED && b.s->end == PEER_END_REMOTE_EXIT);
    peer_session_app_exit(a.s, 11); // closed already: nothing more
    CHECK(peer_session_next_frame(a.s, 11, f, sizeof(f)) == 0);
    // a plain END (no reason) is REMOTE
    peer_session_reset(a.s);
    peer_session_reset(b.s);
    uint32_t t = PEER_INVITE_GAP_MS; // the same peer again: after the invite gap
    open_pair(&a, &b, t);
    peer_session_end(a.s, t + 10);
    n = peer_session_next_frame(a.s, t + 10, f, sizeof(f));
    CHECK(n == PEER_HDR && peer_session_on_frame(b.s, t + 10, 11, 11, f, n) == PEER_RX_OK);
    CHECK(b.s->end == PEER_END_REMOTE);
    free(a.s);
    free(b.s);
}

static void visible_and_rate(void) {
    badge_t       a = {.s = new_session(), .link = 1}, b = {.s = new_session(), .link = 11};
    peer_invite_t inv = invite_of(1, "battleship", "A");
    uint8_t       f[PEER_FRAME_MAX], p[PEER_INVITE_MAX];
    size_t        n = peer_frame_build(PEER_F_INVITE, 0, 0, p, peer_invite_encode(&inv, p, sizeof(p)), f, sizeof(f));
    CHECK(peer_session_on_frame(b.s, 0, 11, 11, f, n) == PEER_RX_REFUSE); // not Visible
    peer_session_set_visible(b.s, 1000, true);
    CHECK(peer_session_visible(b.s, 1000 + PEER_VISIBLE_MS - 1) && !peer_session_visible(b.s, 1000 + PEER_VISIBLE_MS));
    CHECK(peer_session_on_frame(b.s, 1000 + PEER_VISIBLE_MS, 11, 11, f, n) == PEER_RX_REFUSE);
    peer_session_set_visible(b.s, 2000, true);
    CHECK(peer_session_on_frame(b.s, 2000, 11, 11, f, n) == PEER_RX_OK);
    CHECK(peer_session_on_frame(b.s, 2001, 12, 12, f, n) == PEER_RX_REFUSE);  // busy with the first
    CHECK(peer_session_on_frame(b.s, 2001, 11, 11, f, n) == PEER_RX_IGNORED); // its own link: never drop it
    CHECK(peer_session_answer(b.s, 2002, false));
    uint8_t out[PEER_HDR];
    CHECK(b.s->end == PEER_END_DECLINED && peer_session_next_frame(b.s, 2002, out, sizeof(out)) == PEER_HDR);
    CHECK(peer_session_on_frame(b.s, 2003, 12, 12, f, n) == PEER_RX_REFUSE); // closed, not reset yet
    peer_session_reset(b.s);
    CHECK(b.s->state == PEER_IDLE && peer_session_visible(b.s, 2004));
    // per peer: the same inviter again within 10 s is refused, then shown; another peer is shown at once
    CHECK(peer_session_on_frame(b.s, 2000 + PEER_INVITE_GAP_MS - 1, 12, 11, f, n) == PEER_RX_REFUSE);
    CHECK(peer_session_on_frame(b.s, 2000 + PEER_INVITE_GAP_MS, 12, 11, f, n) == PEER_RX_OK);
    uint32_t t = 2000 + PEER_INVITE_GAP_MS;
    peer_session_answer(b.s, t, false);
    peer_session_reset(b.s);
    CHECK(peer_session_on_frame(b.s, t, 13, 13, f, n) == PEER_RX_OK);
    // the table remembers PEER_RATE_SLOTS peers: with all of them shown in the last 10 s, a new one waits
    for (uint32_t k = 100; k < 100 + PEER_RATE_SLOTS - 2; k++) { // 11 and 13 hold two slots already
        peer_session_answer(b.s, t, false);
        peer_session_reset(b.s);
        CHECK(peer_session_on_frame(b.s, t + 1, 14, k, f, n) == PEER_RX_OK);
    }
    peer_session_answer(b.s, t, false);
    peer_session_reset(b.s);
    CHECK(peer_session_on_frame(b.s, t + 1, 14, 999, f, n) == PEER_RX_REFUSE); // table full of recent peers
    CHECK(peer_session_on_frame(b.s, 2000 + 2 * PEER_INVITE_GAP_MS, 14, 999, f, n) == PEER_RX_OK); // 11's slot aged
    CHECK(!peer_session_answer(a.s, 0, true));                                                     // not invited
    peer_session_set_visible(b.s, 0, false);
    CHECK(!peer_session_visible(b.s, 1));
    CHECK(peer_session_on_frame(b.s, 0, 11, 11, f, 3) == PEER_RX_BAD);
    free(a.s);
    free(b.s);
}

static void decline_and_timeouts(void) {
    badge_t       a = {.s = new_session(), .link = 1}, b = {.s = new_session(), .link = 11};
    peer_invite_t inv = invite_of(1, "battleship", "A");
    // declined
    peer_session_set_visible(b.s, 0, true);
    CHECK(peer_session_invite(a.s, 0, 1, &inv) == LICHEN_BT_OK);
    CHECK(peer_session_invite(a.s, 0, 1, &inv) == LICHEN_BT_BUSY);
    pump(&a, &b, 0);
    peer_session_answer(b.s, 10, false);
    pump(&a, &b, 10);
    CHECK(a.s->state == PEER_CLOSED && a.s->end == PEER_END_DECLINED);
    // no answer: 30 s on both sides
    peer_session_reset(a.s);
    peer_session_reset(b.s);
    uint32_t t = 50000;
    CHECK(peer_session_invite(a.s, t, 1, &inv) == LICHEN_BT_OK);
    pump(&a, &b, t);
    peer_session_tick(a.s, t + PEER_ANSWER_MS - 1);
    peer_session_tick(b.s, t + PEER_ANSWER_MS - 1);
    CHECK(a.s->state == PEER_INVITING && b.s->state == PEER_INVITED);
    peer_session_tick(b.s, t + PEER_ANSWER_MS);
    CHECK(b.s->state == PEER_CLOSED && b.s->end == PEER_END_NO_ANSWER);
    CHECK(!peer_session_answer(b.s, t + PEER_ANSWER_MS, true)); // too late
    peer_session_tick(a.s, t + PEER_ANSWER_MS);
    CHECK(a.s->state == PEER_CLOSED && a.s->end == PEER_END_NO_ANSWER);
    // pairing: 30 s, and a failure
    peer_session_reset(a.s);
    peer_session_reset(b.s);
    t = 200000;
    CHECK(peer_session_invite(a.s, t, 1, &inv) == LICHEN_BT_OK);
    pump(&a, &b, t);
    peer_session_answer(b.s, t, true);
    pump(&a, &b, t);
    peer_session_tick(a.s, t + PEER_PAIR_MS - 1);
    CHECK(a.s->state == PEER_PAIRING);
    peer_session_tick(a.s, t + PEER_PAIR_MS);
    CHECK(a.s->state == PEER_CLOSED && a.s->end == PEER_END_PAIR_FAILED);
    peer_session_secured(b.s, t + 1, false);
    CHECK(b.s->state == PEER_CLOSED && b.s->end == PEER_END_PAIR_FAILED);
    peer_session_secured(b.s, t + 2, true); // not pairing any more: nothing
    CHECK(b.s->state == PEER_CLOSED);
    // the link goes down while invited
    peer_session_reset(a.s);
    peer_session_reset(b.s);
    t = 400000;
    CHECK(peer_session_invite(a.s, t, 1, &inv) == LICHEN_BT_OK);
    pump(&a, &b, t);
    peer_session_link_down(b.s, t, 99); // another link: nothing
    CHECK(b.s->state == PEER_INVITED);
    peer_session_link_down(b.s, t, 11);
    CHECK(b.s->state == PEER_CLOSED && b.s->end == PEER_END_LOST);
    CHECK(peer_session_next_frame(b.s, t, (uint8_t[PEER_HDR]){0}, PEER_HDR) == 0); // nothing to send on a dead link
    free(a.s);
    free(b.s);
}

static void window_and_retransmit(void) {
    badge_t a = {.s = new_session(), .link = 1}, b = {.s = new_session(), .link = 11};
    open_pair(&a, &b, 0);
    uint8_t m = 0;
    for (unsigned i = 0; i < PEER_TXQ; i++, m++) {
        CHECK(peer_session_send(a.s, &m, 1) == LICHEN_BT_OK);
    }
    CHECK(peer_session_send(a.s, &m, 1) == LICHEN_BT_BUSY);
    // lost on the way: nothing acked, a resend after the timeout
    a.sent = 0;
    a.drop = true;
    pump(&a, &b, 10);
    CHECK(a.sent == 4 && a.s->txq_n == PEER_TXQ);
    a.drop = false;
    peer_session_tick(a.s, 10 + PEER_RTO_MS - 1);
    pump(&a, &b, 10 + PEER_RTO_MS - 1);
    CHECK(a.sent == 4);
    peer_session_tick(a.s, 10 + PEER_RTO_MS);
    pump(&a, &b, 10 + PEER_RTO_MS);
    CHECK(a.sent == 8 && a.s->txq_n == 0 && a.s->retries == 0);
    CHECK(peer_session_take_events(a.s) & PEER_EV_READY);
    uint8_t got[4];
    for (uint8_t i = 0; i < PEER_TXQ; i++) {
        CHECK(peer_session_recv(b.s, got, sizeof(got)) == 1 && got[0] == i);
    }
    // the ack is lost: the resend is a duplicate, taken once, acked again
    CHECK(peer_session_send(a.s, &m, 1) == LICHEN_BT_OK);
    b.drop = true;
    pump(&a, &b, 5000);
    b.drop = false;
    peer_session_tick(a.s, 5000 + PEER_RTO_MS);
    pump(&a, &b, 5000 + PEER_RTO_MS);
    CHECK(a.s->txq_n == 0);
    CHECK(peer_session_recv(b.s, got, sizeof(got)) == 1 && got[0] == m);
    CHECK(peer_session_recv(b.s, got, sizeof(got)) == -1);
    // never acked: closed after PEER_RETRIES resends
    CHECK(peer_session_send(a.s, &m, 1) == LICHEN_BT_OK);
    a.drop     = true;
    uint32_t t = 10000;
    pump(&a, &b, t);
    for (unsigned i = 0; i < PEER_RETRIES; i++) {
        t += PEER_RTO_MS;
        peer_session_tick(a.s, t);
        CHECK(a.s->state == PEER_OPEN);
        pump(&a, &b, t); // b's keepalives still reach a, so only the retries end it
    }
    t += PEER_RTO_MS;
    peer_session_tick(a.s, t);
    CHECK(a.s->state == PEER_CLOSED && a.s->end == PEER_END_RETRIES);
    free(a.s);
    free(b.s);
}

static void rx_full_and_ack_lies(void) {
    badge_t a = {.s = new_session(), .link = 1}, b = {.s = new_session(), .link = 11};
    open_pair(&a, &b, 0);
    uint8_t m;
    for (m = 0; m < PEER_TXQ; m++) {
        CHECK(peer_session_send(a.s, &m, 1) == LICHEN_BT_OK);
    }
    pump(&a, &b, 1);
    for (; m < 2 * PEER_TXQ; m++) {
        CHECK(peer_session_send(a.s, &m, 1) == LICHEN_BT_OK);
    }
    pump(&a, &b, 2); // b's queue is full: these are not acked
    CHECK(b.s->rxq_n == PEER_RXQ && a.s->txq_n == PEER_TXQ);
    uint8_t got[4];
    for (uint8_t i = 0; i < PEER_RXQ; i++) {
        CHECK(peer_session_recv(b.s, got, sizeof(got)) == 1 && got[0] == i);
    }
    peer_session_tick(a.s, 2 + PEER_RTO_MS);
    pump(&a, &b, 2 + PEER_RTO_MS);
    for (uint8_t i = PEER_TXQ; i < 2 * PEER_TXQ; i++) {
        CHECK(peer_session_recv(b.s, got, sizeof(got)) == 1 && got[0] == i);
    }
    CHECK(a.s->txq_n == 0);
    // an ack for what was never sent is ignored
    CHECK(peer_session_send(a.s, &m, 1) == LICHEN_BT_OK); // queued, not sent
    uint8_t f[PEER_HDR];
    size_t  n = peer_frame_build(PEER_F_ACK, 0, (uint16_t)(a.s->tx_base + 1), NULL, 0, f, sizeof(f));
    CHECK(peer_session_on_frame(a.s, 3, 1, 1, f, n) == PEER_RX_OK);
    CHECK(a.s->txq_n == 1);
    // frames on another link are not the session's
    CHECK(peer_session_on_frame(a.s, 3, 2, 2, f, n) == PEER_RX_IGNORED);
    free(a.s);
    free(b.s);
}

static void seq_wrap(void) {
    badge_t a = {.s = new_session(), .link = 1}, b = {.s = new_session(), .link = 11};
    open_pair(&a, &b, 0);
    a.s->tx_base = 0xfffe; // as if 65534 messages went before
    b.s->rx_next = 0xfffe;
    uint8_t got[4];
    for (uint8_t i = 0; i < 6; i++) { // seq 0xfffe, 0xffff, 0, 1, 2, 3
        CHECK(peer_session_send(a.s, &i, 1) == LICHEN_BT_OK);
        pump(&a, &b, 10u + i);
        CHECK(peer_session_recv(b.s, got, sizeof(got)) == 1 && got[0] == i);
    }
    CHECK(a.s->tx_base == 4 && b.s->rx_next == 4 && a.s->txq_n == 0);
    free(a.s);
    free(b.s);
}

static void keepalive_and_silence(void) {
    badge_t a = {.s = new_session(), .link = 1}, b = {.s = new_session(), .link = 11};
    open_pair(&a, &b, 0);
    uint8_t f[PEER_FRAME_MAX];
    CHECK(peer_session_next_frame(a.s, PEER_KEEPALIVE_MS - 1, f, sizeof(f)) == 0);
    CHECK(peer_session_next_frame(a.s, PEER_KEEPALIVE_MS, f, sizeof(f)) == PEER_HDR && f[1] == PEER_F_ACK);
    CHECK(peer_session_next_frame(a.s, PEER_KEEPALIVE_MS, f, sizeof(f)) == 0);
    // both alive for a minute: no silence
    for (uint32_t t = 1000; t <= 60000; t += 1000) {
        pump(&a, &b, t);
        peer_session_tick(a.s, t);
        peer_session_tick(b.s, t);
    }
    CHECK(a.s->state == PEER_OPEN && b.s->state == PEER_OPEN);
    // b goes quiet: a ends it 20 s after b's last frame
    uint32_t last = b.s->last_tx_ms;
    peer_session_tick(a.s, last + PEER_SILENCE_MS - 1);
    CHECK(a.s->state == PEER_OPEN);
    peer_session_tick(a.s, last + PEER_SILENCE_MS);
    CHECK(a.s->state == PEER_CLOSED && a.s->end == PEER_END_SILENCE);
    free(a.s);
    free(b.s);
}

// Both invite at once: A is central on a1 and peripheral on a2, B the mirror. The connection cap admits it (one
// central and one peripheral each); the larger nonce wins on both sides.
static void glare(uint32_t nonce_a, uint32_t nonce_b) {
    peer_session_t *A = new_session(), *B = new_session();
    ble_conns_t     ca = {0}, cb = {0};
    peer_session_set_visible(A, 0, true);
    peer_session_set_visible(B, 0, true);
    CHECK(ble_conn_add(&ca, LICHEN_BT_ROLE_CENTRAL) && ble_conn_add(&cb, LICHEN_BT_ROLE_CENTRAL));
    CHECK(ble_conn_add(&ca, LICHEN_BT_ROLE_PERIPHERAL) && ble_conn_add(&cb, LICHEN_BT_ROLE_PERIPHERAL));
    peer_invite_t ia = invite_of(nonce_a, "battleship", "A"), ib = invite_of(nonce_b, "battleship", "B");
    CHECK(peer_session_invite(A, 0, 1, &ia) == LICHEN_BT_OK);  // A's a1 = B's b2
    CHECK(peer_session_invite(B, 0, 21, &ib) == LICHEN_BT_OK); // B's b1 = A's a2
    uint8_t   fa[PEER_FRAME_MAX], fb[PEER_FRAME_MAX];
    size_t    na = peer_session_next_frame(A, 1, fa, sizeof(fa)), nb = peer_session_next_frame(B, 1, fb, sizeof(fb));
    peer_rx_t at_b = peer_session_on_frame(B, 2, 22, 22, fa, na);
    peer_rx_t at_a = peer_session_on_frame(A, 2, 2, 2, fb, nb);
    if (nonce_a == nonce_b) {
        CHECK(at_a == PEER_RX_REFUSE && at_b == PEER_RX_REFUSE);
        CHECK(A->state == PEER_INVITING && B->state == PEER_INVITING); // each DECLINE to come ends the other's invite
    } else {
        bool            a_wins = nonce_a > nonce_b;
        peer_session_t *w = a_wins ? A : B, *l = a_wins ? B : A;
        CHECK((a_wins ? at_a : at_b) == PEER_RX_REFUSE);
        CHECK((a_wins ? at_b : at_a) == PEER_RX_YIELD);
        CHECK(w->state == PEER_INVITING && w->role == LICHEN_BT_ROLE_CENTRAL);
        CHECK(l->state == PEER_INVITED && l->role == LICHEN_BT_ROLE_PERIPHERAL);
        CHECK(l->yielded_link == (a_wins ? 21 : 1) && l->link == (a_wins ? 22 : 2));
        CHECK(l->invite.nonce == (a_wins ? nonce_a : nonce_b));
        CHECK(peer_session_next_frame(l, 3, fa, sizeof(fa)) == 0); // its own INVITE is dropped with the link
    }
    // while the session's central link is up, a badge with an app's central link too would be past the cap
    ble_conns_t full = {.central = 1, .peripheral = 0};
    CHECK(!ble_conn_may(&full, LICHEN_BT_ROLE_CENTRAL)); // the service refuses to invite (FULL)
    free(A);
    free(B);
}

int main(void) {
    frames();
    invites();
    fuzz();
    happy_path();
    visible_and_rate();
    app_exit();
    decline_and_timeouts();
    window_and_retransmit();
    rx_full_and_ack_lies();
    seq_wrap();
    keepalive_and_silence();
    glare(5, 9);
    glare(9, 5);
    glare(7, 7);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("peer_session_test: all passed\n");
    return 0;
}
