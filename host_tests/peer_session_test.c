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

// The constants are the wire contract of lichen_bt.h (the A7 test peer implements it)
static void wire_contract(void) {
    CHECK(PEER_HDR == 8 && PEER_VERSION == 1 && PEER_TXQ == LICHEN_BT_PEER_WINDOW && PEER_INVITE_MAX == 64);
    uint8_t b[PEER_HDR + 2];
    size_t  n      = peer_frame_build(PEER_F_DATA, 0x0201, 0x0403, "xy", 2, b, sizeof(b));
    uint8_t want[] = {1, LICHEN_BT_PEER_F_DATA, 0x01, 0x02, 0x03, 0x04, 2, 0, 'x', 'y'};
    CHECK(n == sizeof(want) && memcmp(b, want, n) == 0);
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

// The service's keys for the two badges (each badge's id of the other)
#define KEY_A 0xa000u
#define KEY_B 0xb000u

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
                peer_session_on_frame(b->s, now, b->link, KEY_A, buf, na);
            }
        }
        size_t nb = peer_session_next_frame(b->s, now, buf, sizeof(buf));
        if (nb) {
            b->sent++;
            if (!b->drop) {
                peer_session_on_frame(a->s, now, a->link, KEY_B, buf, nb);
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
    CHECK(peer_session_invite(a->s, t, a->link, KEY_B, &inv) == LICHEN_BT_OK);
    pump(a, b, t);
    CHECK(b->s->state == PEER_INVITED && (peer_session_take_events(b->s) & PEER_EV_INVITE));
    CHECK(peer_session_answer(b->s, t, true));
    pump(a, b, t);
    CHECK(a->s->state == PEER_PAIRING && (peer_session_take_events(a->s) & PEER_EV_PAIR));
    CHECK(b->s->state == PEER_PAIRING);
    peer_session_secured(a->s, t, a->link, true);
    peer_session_secured(b->s, t, b->link, true);
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
    // L6: the decline cools every invite down for 10 s, whoever sends it
    CHECK(peer_session_on_frame(b.s, 2002 + PEER_INVITE_COOLDOWN_MS - 1, 12, 13, f, n) == PEER_RX_REFUSE);
    CHECK(peer_session_on_frame(b.s, 2002 + PEER_INVITE_COOLDOWN_MS, 12, 13, f, n) == PEER_RX_OK);
    // so does an invite nobody answered
    peer_session_tick(b.s, 2002 + PEER_INVITE_COOLDOWN_MS + PEER_ANSWER_MS);
    CHECK(b.s->end == PEER_END_NO_ANSWER);
    peer_session_reset(b.s);
    uint32_t t = 2002 + PEER_INVITE_COOLDOWN_MS + PEER_ANSWER_MS;
    CHECK(peer_session_on_frame(b.s, t + PEER_INVITE_COOLDOWN_MS - 1, 14, 14, f, n) == PEER_RX_REFUSE);
    CHECK(peer_session_on_frame(b.s, t + PEER_INVITE_COOLDOWN_MS, 14, 14, f, n) == PEER_RX_OK);
    free(b.s);
    // per peer, with links that went down (no cooldown): the same inviter within 10 s is refused, another is shown
    b.s = new_session();
    peer_session_set_visible(b.s, 0, true);
    CHECK(peer_session_on_frame(b.s, 0, 1, 11, f, n) == PEER_RX_OK);
    peer_session_link_down(b.s, 1, 1);
    peer_session_reset(b.s);
    CHECK(peer_session_on_frame(b.s, PEER_INVITE_GAP_MS - 1, 2, 11, f, n) == PEER_RX_REFUSE);
    CHECK(peer_session_on_frame(b.s, PEER_INVITE_GAP_MS - 1, 2, 13, f, n) == PEER_RX_OK);
    peer_session_link_down(b.s, PEER_INVITE_GAP_MS, 2);
    peer_session_reset(b.s);
    CHECK(peer_session_on_frame(b.s, PEER_INVITE_GAP_MS, 3, 11, f, n) == PEER_RX_OK);
    free(b.s);
    // the table remembers PEER_RATE_SLOTS peers: with all of them shown in the last 10 s, a new one waits
    b.s = new_session();
    peer_session_set_visible(b.s, 0, true);
    for (uint32_t k = 100; k < 100 + PEER_RATE_SLOTS; k++) {
        CHECK(peer_session_on_frame(b.s, 1, 4, k, f, n) == PEER_RX_OK);
        peer_session_link_down(b.s, 1, 4);
        peer_session_reset(b.s);
    }
    CHECK(peer_session_on_frame(b.s, 1, 4, 999, f, n) == PEER_RX_REFUSE);                  // full of recent peers
    CHECK(peer_session_on_frame(b.s, 1 + PEER_INVITE_GAP_MS, 4, 999, f, n) == PEER_RX_OK); // the slots aged
    CHECK(!peer_session_answer(a.s, 0, true));                                             // not invited
    peer_session_set_visible(b.s, 0, false);
    CHECK(!peer_session_visible(b.s, 1));
    // Visible running out is latched by the tick: the same counter value after a wrap is not Visible again
    peer_session_set_visible(b.s, 100, true);
    peer_session_tick(b.s, 100 + PEER_VISIBLE_MS);
    CHECK(!b.s->visible && !peer_session_visible(b.s, 100));
    CHECK(peer_session_on_frame(b.s, 0, 11, 11, f, 3) == PEER_RX_BAD);
    free(a.s);
    free(b.s);
}

static void decline_and_timeouts(void) {
    badge_t       a = {.s = new_session(), .link = 1}, b = {.s = new_session(), .link = 11};
    peer_invite_t inv = invite_of(1, "battleship", "A");
    // declined
    peer_session_set_visible(b.s, 0, true);
    CHECK(peer_session_invite(a.s, 0, 1, KEY_B, &inv) == LICHEN_BT_OK);
    CHECK(peer_session_invite(a.s, 0, 1, KEY_B, &inv) == LICHEN_BT_BUSY);
    pump(&a, &b, 0);
    peer_session_answer(b.s, 10, false);
    pump(&a, &b, 10);
    CHECK(a.s->state == PEER_CLOSED && a.s->end == PEER_END_DECLINED);
    // no answer: 30 s on both sides
    peer_session_reset(a.s);
    peer_session_reset(b.s);
    uint32_t t = 50000;
    CHECK(peer_session_invite(a.s, t, 1, KEY_B, &inv) == LICHEN_BT_OK);
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
    CHECK(peer_session_invite(a.s, t, 1, KEY_B, &inv) == LICHEN_BT_OK);
    pump(&a, &b, t);
    peer_session_answer(b.s, t, true);
    pump(&a, &b, t);
    peer_session_tick(a.s, t + PEER_PAIR_MS - 1);
    CHECK(a.s->state == PEER_PAIRING);
    peer_session_tick(a.s, t + PEER_PAIR_MS);
    CHECK(a.s->state == PEER_CLOSED && a.s->end == PEER_END_PAIR_FAILED);
    peer_session_secured(b.s, t + 1, 11, false);
    CHECK(b.s->state == PEER_CLOSED && b.s->end == PEER_END_PAIR_FAILED);
    peer_session_secured(b.s, t + 2, 11, true); // not pairing any more: nothing
    CHECK(b.s->state == PEER_CLOSED);
    // the link goes down while invited
    peer_session_reset(a.s);
    peer_session_reset(b.s);
    t = 400000;
    CHECK(peer_session_invite(a.s, t, 1, KEY_B, &inv) == LICHEN_BT_OK);
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

// A resend is idempotent at the receiver: the same seq twice is acked again and dropped, never delivered twice, also
// after the app read the first copy
static void duplicate_once(void) {
    badge_t a = {.s = new_session(), .link = 1}, b = {.s = new_session(), .link = 11};
    open_pair(&a, &b, 0);
    uint8_t f[PEER_FRAME_MAX], got[4];
    size_t  n = peer_frame_build(PEER_F_DATA, 0, 0, "m", 1, f, sizeof(f));
    CHECK(peer_session_on_frame(b.s, 1, 11, KEY_A, f, n) == PEER_RX_OK);
    CHECK(b.s->rxq_n == 1 && b.s->rx_next == 1 && (peer_session_take_events(b.s) & PEER_EV_MSG));
    CHECK(peer_session_on_frame(b.s, 2, 11, KEY_A, f, n) == PEER_RX_OK);
    CHECK(b.s->rxq_n == 1 && b.s->rx_next == 1 && !(peer_session_take_events(b.s) & PEER_EV_MSG));
    uint8_t ack[PEER_FRAME_MAX];
    CHECK(peer_session_next_frame(b.s, 2, ack, sizeof(ack)) == PEER_HDR && ack[1] == PEER_F_ACK && ack[4] == 1);
    CHECK(peer_session_recv(b.s, got, sizeof(got)) == 1 && got[0] == 'm');
    CHECK(peer_session_on_frame(b.s, 3, 11, KEY_A, f, n) == PEER_RX_OK); // once more, after the read
    CHECK(peer_session_recv(b.s, got, sizeof(got)) == -1 && b.s->rx_next == 1);
    CHECK(peer_session_next_frame(b.s, 3, ack, sizeof(ack)) == PEER_HDR && ack[4] == 1); // acked again
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
// central and one peripheral each). The larger nonce wins on both sides whichever frame arrives first, and the
// surviving invite runs through to OPEN with the winner as its central.
typedef struct {
    peer_session_t *A, *B;
    uint8_t         fa[PEER_FRAME_MAX], fb[PEER_FRAME_MAX]; // A's INVITE (on a1 = b2), B's (on b1 = a2)
    size_t          na, nb;
} glare_t;

static void glare_start(glare_t *g, uint32_t nonce_a, uint32_t nonce_b) {
    g->A = new_session();
    g->B = new_session();
    peer_session_set_visible(g->A, 0, true);
    peer_session_set_visible(g->B, 0, true);
    peer_invite_t ia = invite_of(nonce_a, "battleship", "A"), ib = invite_of(nonce_b, "battleship", "B");
    CHECK(peer_session_invite(g->A, 0, 1, KEY_B, &ia) == LICHEN_BT_OK);
    CHECK(peer_session_invite(g->B, 0, 21, KEY_A, &ib) == LICHEN_BT_OK);
    g->na = peer_session_next_frame(g->A, 1, g->fa, sizeof(g->fa));
    g->nb = peer_session_next_frame(g->B, 1, g->fb, sizeof(g->fb));
}

// From the INVITED side: accept, pair, open; w is central on wlink, l peripheral on llink
static void glare_finish(peer_session_t *w, uint16_t wlink, uint32_t wkey, peer_session_t *l, uint16_t llink) {
    CHECK(l->state == PEER_INVITED && l->link == llink);
    CHECK(peer_session_answer(l, 5, true));
    uint8_t f[PEER_FRAME_MAX];
    size_t  n = peer_session_next_frame(l, 5, f, sizeof(f));
    CHECK(peer_session_on_frame(w, 5, wlink, wkey, f, n) == PEER_RX_OK && w->state == PEER_PAIRING);
    peer_session_secured(w, 6, wlink, true);
    peer_session_secured(l, 6, llink, true);
    CHECK(w->state == PEER_OPEN && l->state == PEER_OPEN);
    CHECK(w->role == LICHEN_BT_ROLE_CENTRAL && l->role == LICHEN_BT_ROLE_PERIPHERAL);
    CHECK(peer_session_send(w, "hit", 4) == LICHEN_BT_OK);
}

// The INVITEs cross: each side sees the other's INVITE while inviting
static void glare_crossing(uint32_t nonce_a, uint32_t nonce_b) {
    glare_t g;
    uint8_t d[PEER_HDR + 1];
    glare_start(&g, nonce_a, nonce_b);
    peer_rx_t at_b = peer_session_on_frame(g.B, 2, 22, KEY_A, g.fa, g.na);
    size_t    db   = at_b == PEER_RX_REFUSE ? peer_session_refusal(g.B, d, sizeof(d)) : 0;
    peer_rx_t at_a = peer_session_on_frame(g.A, 2, 2, KEY_B, g.fb, g.nb);
    if (nonce_a == nonce_b) {
        CHECK(at_a == PEER_RX_REFUSE && at_b == PEER_RX_REFUSE);
        CHECK(db == PEER_HDR); // a plain DECLINE: no glare reason, so both invites end
        CHECK(peer_session_on_frame(g.A, 3, 1, KEY_B, d, db) == PEER_RX_OK);
        CHECK(g.A->state == PEER_CLOSED && g.A->end == PEER_END_DECLINED);
    } else if (nonce_a > nonce_b) {
        CHECK(at_a == PEER_RX_REFUSE && at_b == PEER_RX_YIELD);
        CHECK(g.B->yielded_link == 21 && (peer_session_take_events(g.B) & PEER_EV_YIELD));
        CHECK(peer_session_next_frame(g.B, 3, d, sizeof(d)) == 0); // its own INVITE is dropped with the link
        size_t da = peer_session_refusal(g.A, d, sizeof(d));
        CHECK(da == PEER_HDR + 1 && d[PEER_HDR] == PEER_WHY_GLARE);
        CHECK(peer_session_on_frame(g.B, 3, 21, KEY_A, d, da) == PEER_RX_IGNORED); // the yielded link: stale
        glare_finish(g.A, 1, KEY_B, g.B, 22);
    } else {
        CHECK(at_b == PEER_RX_REFUSE && at_a == PEER_RX_YIELD);
        glare_finish(g.B, 21, KEY_A, g.A, 2);
    }
    free(g.A);
    free(g.B);
}

// The winner's DECLINE reaches the loser before the winner's INVITE does
static void glare_decline_first(void) {
    glare_t g;
    uint8_t d[PEER_HDR + 1];
    glare_start(&g, 9, 5); // A wins
    CHECK(peer_session_on_frame(g.A, 2, 2, KEY_B, g.fb, g.nb) == PEER_RX_REFUSE);
    size_t da = peer_session_refusal(g.A, d, sizeof(d));
    CHECK(peer_session_on_frame(g.B, 3, 21, KEY_A, d, da) == PEER_RX_YIELD);
    CHECK(g.B->state == PEER_IDLE && g.B->yielded_link == 21 && (peer_session_take_events(g.B) & PEER_EV_YIELD));
    CHECK(peer_session_on_frame(g.B, 4, 22, KEY_A, g.fa, g.na) == PEER_RX_OK); // A's INVITE now
    glare_finish(g.A, 1, KEY_B, g.B, 22);
    free(g.A);
    free(g.B);
    // a third badge's invite while we invite is refused, whatever its nonce: no yield to a stranger
    glare_start(&g, 5, 9);
    CHECK(peer_session_on_frame(g.A, 2, 7, 0xc000u, g.fb, g.nb) == PEER_RX_REFUSE);
    CHECK(peer_session_refusal(g.A, d, sizeof(d)) == PEER_HDR); // busy, not a glare
    CHECK(g.A->state == PEER_INVITING && g.A->link == 1);
    free(g.A);
    free(g.B);
}

// M1: a lost glare by DECLINE waits for the winner's invite PEER_ANSWER_MS at most, so a lying DECLINE + GLARE cannot
// park the session; the awaited invite needs no Visible
static void glare_wait_ends(void) {
    glare_t g;
    uint8_t d[PEER_HDR + 1];
    glare_start(&g, 9, 5);
    CHECK(peer_session_on_frame(g.A, 2, 2, KEY_B, g.fb, g.nb) == PEER_RX_REFUSE);
    size_t da = peer_session_refusal(g.A, d, sizeof(d));
    CHECK(peer_session_on_frame(g.B, 3, 21, KEY_A, d, da) == PEER_RX_YIELD);
    peer_session_take_events(g.B);
    peer_session_tick(g.B, 3 + PEER_ANSWER_MS - 1);
    CHECK(g.B->state == PEER_IDLE && g.B->await_glare);
    peer_session_tick(g.B, 3 + PEER_ANSWER_MS);
    CHECK(g.B->state == PEER_CLOSED && g.B->end == PEER_END_NO_ANSWER && !g.B->await_glare);
    CHECK(peer_session_take_events(g.B) & PEER_EV_CLOSED);
    free(g.A);
    free(g.B);
    glare_start(&g, 9, 5);
    peer_session_on_frame(g.A, 2, 2, KEY_B, g.fb, g.nb);
    da = peer_session_refusal(g.A, d, sizeof(d));
    peer_session_on_frame(g.B, 3, 21, KEY_A, d, da);
    peer_session_set_visible(g.B, 4, false);
    CHECK(peer_session_on_frame(g.B, 5, 22, KEY_A, g.fa, g.na) == PEER_RX_OK && g.B->state == PEER_INVITED);
    free(g.A);
    free(g.B);
}

// L4: only the session's own link opens it
static void secured_link(void) {
    badge_t a = {.s = new_session(), .link = 1}, b = {.s = new_session(), .link = 11};
    peer_session_set_visible(b.s, 0, true);
    peer_invite_t inv = invite_of(1, "battleship", "A");
    CHECK(peer_session_invite(a.s, 0, 1, KEY_B, &inv) == LICHEN_BT_OK);
    pump(&a, &b, 0);
    peer_session_answer(b.s, 1, true);
    pump(&a, &b, 1);
    peer_session_secured(a.s, 2, 7, true); // another link's pairing
    CHECK(a.s->state == PEER_PAIRING);
    peer_session_secured(a.s, 2, 7, false);
    CHECK(a.s->state == PEER_PAIRING);
    peer_session_secured(a.s, 2, 1, true);
    CHECK(a.s->state == PEER_OPEN);
    free(a.s);
    free(b.s);
}

static void glare_cap(void) {
    ble_conns_t c = {0};
    CHECK(ble_conn_add(&c, LICHEN_BT_ROLE_CENTRAL) && ble_conn_add(&c, LICHEN_BT_ROLE_PERIPHERAL)); // a glare's two
    CHECK(!ble_conn_may(&c, LICHEN_BT_ROLE_CENTRAL)); // no second central: the service refuses to invite (FULL)
    CHECK(ble_conn_may(&c, LICHEN_BT_ROLE_PERIPHERAL));
}

// Before both sides are secured, DATA is not taken (the retransmission brings it later); END and a lost link while
// pairing end the session
static void pairing_edges(void) {
    badge_t a = {.s = new_session(), .link = 1}, b = {.s = new_session(), .link = 11};
    peer_session_set_visible(b.s, 0, true);
    peer_invite_t inv = invite_of(1, "battleship", "A");
    CHECK(peer_session_invite(a.s, 0, 1, KEY_B, &inv) == LICHEN_BT_OK);
    pump(&a, &b, 0);
    peer_session_answer(b.s, 1, true);
    pump(&a, &b, 1);
    CHECK(a.s->state == PEER_PAIRING && b.s->state == PEER_PAIRING);
    uint8_t f[PEER_FRAME_MAX];
    size_t  n = peer_frame_build(PEER_F_DATA, 0, 0, "x", 1, f, sizeof(f));
    CHECK(peer_session_on_frame(b.s, 2, 11, KEY_A, f, n) == PEER_RX_IGNORED);
    CHECK(b.s->rxq_n == 0 && b.s->rx_next == 0);
    CHECK(peer_session_send(a.s, "x", 1) == LICHEN_BT_NOT_CONNECTED);
    // the central opens first and sends; the peripheral is secured a moment later and takes the resend
    peer_session_secured(a.s, 3, 1, true);
    CHECK(peer_session_send(a.s, "x", 1) == LICHEN_BT_OK);
    pump(&a, &b, 3);
    CHECK(b.s->rxq_n == 0);
    peer_session_secured(b.s, 4, 11, true);
    peer_session_tick(a.s, 3 + PEER_RTO_MS);
    pump(&a, &b, 3 + PEER_RTO_MS);
    uint8_t got[2];
    CHECK(peer_session_recv(b.s, got, sizeof(got)) == 1 && got[0] == 'x');
    free(a.s);
    free(b.s);
    // END while pairing, and the link lost while pairing
    a.s = new_session();
    b.s = new_session();
    peer_session_set_visible(b.s, 0, true);
    CHECK(peer_session_invite(a.s, 0, 1, KEY_B, &inv) == LICHEN_BT_OK);
    pump(&a, &b, 0);
    peer_session_answer(b.s, 1, true);
    pump(&a, &b, 1);
    peer_session_end(b.s, 2);
    pump(&a, &b, 2);
    CHECK(a.s->state == PEER_CLOSED && a.s->end == PEER_END_REMOTE);
    peer_session_secured(a.s, 3, 1, true); // too late: nothing
    CHECK(a.s->state == PEER_CLOSED);
    peer_session_reset(a.s);
    peer_session_reset(b.s);
    uint32_t t = PEER_INVITE_GAP_MS;
    CHECK(peer_session_invite(a.s, t, 1, KEY_B, &inv) == LICHEN_BT_OK);
    pump(&a, &b, t);
    peer_session_answer(b.s, t, true);
    pump(&a, &b, t);
    peer_session_link_down(a.s, t + 1, 1);
    CHECK(a.s->state == PEER_CLOSED && a.s->end == PEER_END_LOST);
    free(a.s);
    free(b.s);
}

// One random frame: any of the six types or a reserved one (7), lengths in and out of the rules, seq and ack near
// the session's counters or anywhere, then sometimes mutated
static size_t random_frame(mut_rng_t *r, peer_session_t const *s, uint8_t *f, size_t cap) {
    uint8_t  pl[PEER_MSG_MAX], tmp[PEER_FRAME_MAX];
    unsigned type = 1 + (unsigned)mut_below(r, 7);
    size_t   len  = 0;
    if (type == PEER_F_INVITE) {
        peer_invite_t inv = invite_of(mut_next(r), "battleship", "R");
        len               = peer_invite_encode(&inv, pl, sizeof(pl));
    } else if (type == PEER_F_DATA) {
        len = 1 + mut_below(r, 64);
        memset(pl, (int)mut_below(r, 256), len);
    } else if (type == PEER_F_DECLINE || type == PEER_F_END) {
        len   = mut_below(r, 2);
        pl[0] = (uint8_t)mut_below(r, 3);
    }
    uint16_t seq = (uint16_t)(mut_next(r) % 3 ? s->rx_next + mut_below(r, 3) - 1 : mut_next(r));
    uint16_t ack = (uint16_t)(mut_next(r) % 2 ? s->tx_base + mut_below(r, 6) : mut_next(r));
    size_t   n   = type == 7 ? 0 : peer_frame_build((peer_ftype_t)type, seq, ack, pl, len, tmp, sizeof(tmp));
    if (type == 7) { // reserved: built by hand, the builder refuses it
        uint8_t h[PEER_HDR] = {PEER_VERSION, 7, 0, 0, 0, 0, 0, 0};
        memcpy(tmp, h, sizeof(h));
        n = sizeof(h);
    }
    return mut_next(r) % 3 ? mut_truncate(tmp, n, n, f, cap) : mut_random(r, tmp, n, f, cap);
}

static void check_bounds(peer_session_t const *s) {
    CHECK(s->state <= PEER_CLOSED && s->end <= PEER_END_REMOTE_EXIT);
    CHECK(s->rxq_n <= PEER_RXQ && s->txq_n <= PEER_TXQ && s->rxq_head < PEER_RXQ);
    CHECK(s->tx_sent <= s->txq_n && s->tx_ever <= s->txq_n);
}

// Drives b (the invitee) or a (the inviter) into st; the session under test
static peer_session_t *drive(badge_t *a, badge_t *b, peer_state_t st) {
    peer_session_set_visible(b->s, 0, true);
    peer_invite_t inv = invite_of(1, "battleship", "A");
    CHECK(peer_session_invite(a->s, 0, a->link, KEY_B, &inv) == LICHEN_BT_OK);
    if (st == PEER_INVITING) {
        return a->s;
    }
    pump(a, b, 0);
    if (st == PEER_INVITED) {
        return b->s;
    }
    peer_session_answer(b->s, 1, true);
    pump(a, b, 1);
    if (st == PEER_PAIRING) {
        return b->s;
    }
    peer_session_secured(a->s, 2, a->link, true);
    peer_session_secured(b->s, 2, b->link, true);
    if (st == PEER_CLOSED) {
        peer_session_end(b->s, 3);
    }
    return b->s;
}

// L5: random frames, on the session's link and others, from the invited badge and strangers, against a session in
// each state; with sends, reads and ticks in between. Nothing leaves its bounds (ASan and UBSan watch the rest).
static void fuzz_states(void) {
    peer_state_t const states[] = {PEER_INVITING, PEER_INVITED, PEER_PAIRING, PEER_OPEN, PEER_CLOSED};
    uint32_t const     keys[]   = {KEY_A, KEY_B, 0x5555u};
    for (size_t k = 0; k < sizeof(states) / sizeof(states[0]); k++) {
        badge_t         a = {.s = new_session(), .link = 1}, b = {.s = new_session(), .link = 11};
        peer_session_t *s    = drive(&a, &b, states[k]);
        peer_session_t *snap = new_session();
        *snap                = *s;
        CHECK(s->state == states[k]);
        mut_rng_t r;
        mut_seed(&r, 777u + (uint32_t)k);
        uint8_t f[PEER_FRAME_MAX + 16], got[PEER_MSG_MAX];
        for (uint32_t i = 0; i < 4000; i++) {
            if (i % 8 == 0) {
                *s = *snap; // back to the state under test every few frames
            }
            uint32_t t    = 10 + i;
            size_t   n    = random_frame(&r, s, f, sizeof(f));
            uint8_t *x    = mut_exact(f, n);
            uint16_t link = mut_next(&r) % 2 ? s->link : (uint16_t)(s->link + 1);
            if (!x) {
                abort();
            }
            peer_session_on_frame(s, t, link, keys[mut_below(&r, 3)], x, n);
            free(x);
            switch (mut_next(&r) % 6) {
                case 0: peer_session_send(s, got, 1 + mut_below(&r, 8)); break;
                case 1: peer_session_recv(s, got, sizeof(got)); break;
                case 2: peer_session_tick(s, t + (uint32_t)mut_below(&r, 40000)); break;
                case 3: peer_session_answer(s, t, mut_next(&r) % 2); break;
                default: break;
            }
            while (peer_session_next_frame(s, t, f, sizeof(f))) {
            }
            peer_session_refusal(s, f, sizeof(f));
            check_bounds(s);
        }
        free(snap);
        free(a.s);
        free(b.s);
    }
}

int main(void) {
    wire_contract();
    frames();
    invites();
    fuzz();
    happy_path();
    visible_and_rate();
    app_exit();
    decline_and_timeouts();
    window_and_retransmit();
    duplicate_once();
    rx_full_and_ack_lies();
    seq_wrap();
    keepalive_and_silence();
    glare_crossing(5, 9);
    glare_crossing(9, 5);
    glare_crossing(7, 7);
    glare_decline_first();
    glare_cap();
    glare_wait_ends();
    secured_link();
    pairing_edges();
    fuzz_states();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("peer_session_test: all passed\n");
    return 0;
}
