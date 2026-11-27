#include "peer_session.h"

#include <string.h>

static uint32_t since(uint32_t now_ms, uint32_t then_ms) {
    return now_ms - then_ms;
}

static uint16_t rd16(uint8_t const *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

// ---- Frames ----

// The payload lengths a type may carry
static bool frame_len_ok(peer_ftype_t type, size_t len) {
    switch (type) {
        case PEER_F_INVITE: return len >= 1 && len <= PEER_INVITE_MAX;
        case PEER_F_DATA: return len >= 1 && len <= PEER_MSG_MAX;
        case PEER_F_ACCEPT:
        case PEER_F_DECLINE:
        case PEER_F_ACK: return len == 0;
        case PEER_F_END: return len <= 1; // an optional reason byte (PEER_WHY_*)
    }
    return false;
}

bool peer_frame_parse(uint8_t const *buf, size_t len, peer_frame_t *out) {
    if (len < PEER_HDR || buf[0] != PEER_VERSION) {
        return false;
    }
    peer_ftype_t type = (peer_ftype_t)buf[1];
    uint16_t     plen = rd16(buf + 6);
    if (len != PEER_HDR + (size_t)plen || !frame_len_ok(type, plen)) {
        return false;
    }
    *out = (peer_frame_t){.type    = type,
                          .seq     = rd16(buf + 2),
                          .ack     = rd16(buf + 4),
                          .len     = plen,
                          .payload = buf + PEER_HDR};
    return true;
}

size_t peer_frame_build(
    peer_ftype_t type, uint16_t seq, uint16_t ack, void const *payload, size_t len, uint8_t *out, size_t cap
) {
    if (!frame_len_ok(type, len) || cap < PEER_HDR + len) {
        return 0;
    }
    out[0] = PEER_VERSION;
    out[1] = (uint8_t)type;
    wr16(out + 2, seq);
    wr16(out + 4, ack);
    wr16(out + 6, (uint16_t)len);
    if (len) {
        memcpy(out + PEER_HDR, payload, len);
    }
    return PEER_HDR + len;
}

// ---- Invites ----

static bool app_char_ok(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static bool name_char_ok(uint8_t c) {
    return c >= 0x20 && c != 0x7f; // UTF-8 bytes pass; the screen draws the name in a one-line label
}

static bool bytes_ok(uint8_t const *p, size_t n, bool (*ok)(uint8_t)) {
    for (size_t i = 0; i < n; i++) {
        if (!ok(p[i])) {
            return false;
        }
    }
    return true;
}

// strlen bounded by the field (strnlen is POSIX, not C99)
static size_t field_len(char const *s, size_t cap) {
    size_t n = 0;
    while (n < cap && s[n]) {
        n++;
    }
    return n;
}

size_t peer_invite_encode(peer_invite_t const *inv, uint8_t *out, size_t cap) {
    size_t al = field_len(inv->app, sizeof(inv->app)), nl = field_len(inv->name, sizeof(inv->name));
    size_t total = 4 + 1 + al + 2 + 1 + nl + 1 + inv->param_len;
    if (al == 0 || al > PEER_APP_MAX || nl > PEER_NAME_MAX || inv->param_len > PEER_PARAM_MAX ||
        total > PEER_INVITE_MAX || total > cap || !bytes_ok((uint8_t const *)inv->app, al, app_char_ok) ||
        !bytes_ok((uint8_t const *)inv->name, nl, name_char_ok)) {
        return 0;
    }
    uint8_t *p = out;
    for (int i = 0; i < 4; i++) {
        *p++ = (uint8_t)(inv->nonce >> (8 * i));
    }
    *p++ = (uint8_t)al;
    memcpy(p, inv->app, al);
    p    += al;
    *p++  = (uint8_t)inv->app_version;
    *p++  = (uint8_t)(inv->app_version >> 8);
    *p++  = (uint8_t)nl;
    memcpy(p, inv->name, nl);
    p    += nl;
    *p++  = inv->param_len;
    memcpy(p, inv->param, inv->param_len);
    return total;
}

// One length-prefixed field of at most max bytes, each passing ok (NULL: any): its length (*at its first byte), or -1
static int take_field(uint8_t const **p, uint8_t const *end, size_t max, bool (*ok)(uint8_t), uint8_t const **at) {
    if (*p >= end) {
        return -1;
    }
    size_t n = **p;
    (*p)++;
    if (n > max || n > (size_t)(end - *p) || (ok && !bytes_ok(*p, n, ok))) {
        return -1;
    }
    *at  = *p;
    *p  += n;
    return (int)n;
}

bool peer_invite_decode(uint8_t const *buf, size_t len, peer_invite_t *out) {
    if (len < 4 || len > PEER_INVITE_MAX) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    uint8_t const *p = buf + 4, *end = buf + len, *a = NULL, *n = NULL, *q = NULL;
    int            al = take_field(&p, end, PEER_APP_MAX, app_char_ok, &a);
    if (al <= 0 || end - p < 2) {
        return false;
    }
    out->app_version  = (uint16_t)(p[0] | (p[1] << 8));
    p                += 2;
    int nl            = take_field(&p, end, PEER_NAME_MAX, name_char_ok, &n);
    int ql            = nl >= 0 ? take_field(&p, end, PEER_PARAM_MAX, NULL, &q) : -1;
    if (nl < 0 || ql < 0 || p != end) {
        return false;
    }
    out->nonce = (uint32_t)buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 24;
    memcpy(out->app, a, (size_t)al);
    memcpy(out->name, n, (size_t)nl);
    memcpy(out->param, q, (size_t)ql);
    out->param_len = (uint8_t)ql;
    return true;
}

// ---- The session ----

void peer_session_init(peer_session_t *s) {
    memset(s, 0, sizeof(*s));
}

void peer_session_set_visible(peer_session_t *s, uint32_t now_ms, bool on) {
    s->visible    = on;
    s->visible_ms = now_ms;
}

bool peer_session_visible(peer_session_t const *s, uint32_t now_ms) {
    return s->visible && since(now_ms, s->visible_ms) < PEER_VISIBLE_MS;
}

static bool active(peer_session_t const *s) {
    return s->state != PEER_IDLE && s->state != PEER_CLOSED;
}

static void enter(peer_session_t *s, peer_state_t st, uint32_t now_ms) {
    s->state    = st;
    s->state_ms = now_ms;
}

// To CLOSED for why; ctl is the last frame to send (END, DECLINE) or 0. Unread messages stay for recv.
static void close_as(peer_session_t *s, uint32_t now_ms, peer_end_t why, uint8_t ctl) {
    enter(s, PEER_CLOSED, now_ms);
    s->end      = why;
    s->ctl      = ctl;
    s->txq_n    = 0;
    s->tx_sent  = 0;
    s->tx_ever  = 0;
    s->ack_due  = false;
    s->why      = 0;
    s->events  |= PEER_EV_CLOSED;
}

lichen_bt_err_t peer_session_invite(peer_session_t *s, uint32_t now_ms, uint16_t link, peer_invite_t const *inv) {
    uint8_t tmp[PEER_INVITE_MAX];
    if (s->state != PEER_IDLE) {
        return LICHEN_BT_BUSY;
    }
    if (peer_invite_encode(inv, tmp, sizeof(tmp)) == 0) {
        return LICHEN_BT_INVALID;
    }
    s->invite     = *inv;
    s->link       = link;
    s->role       = LICHEN_BT_ROLE_CENTRAL;
    s->end        = PEER_END_NONE;
    s->ctl        = PEER_F_INVITE;
    s->last_rx_ms = now_ms;
    enter(s, PEER_INVITING, now_ms);
    return LICHEN_BT_OK;
}

static void take_invite(peer_session_t *s, uint32_t now_ms, uint16_t link, peer_invite_t const *inv) {
    s->invite      = *inv;
    s->link        = link;
    s->role        = LICHEN_BT_ROLE_PERIPHERAL;
    s->end         = PEER_END_NONE;
    s->ctl         = 0;
    s->last_rx_ms  = now_ms;
    s->events     |= PEER_EV_INVITE;
    enter(s, PEER_INVITED, now_ms);
}

// Per peer (the service's key for the inviter's address), one invite shown per PEER_INVITE_GAP_MS. The table is
// bounded: a peer not in it takes a free slot or one older than the gap, and is refused when there is none.
static bool rate_take(peer_session_t *s, uint32_t now_ms, uint32_t key) {
    int slot = -1;
    for (int i = 0; i < (int)PEER_RATE_SLOTS; i++) {
        peer_rate_t const *r = &s->rate[i];
        if (r->used && r->key == key) {
            if (since(now_ms, r->ms) < PEER_INVITE_GAP_MS) {
                return false;
            }
            slot = i;
            break;
        }
        if (slot < 0 && (!r->used || since(now_ms, r->ms) >= PEER_INVITE_GAP_MS)) {
            slot = i;
        }
    }
    if (slot < 0) {
        return false;
    }
    s->rate[slot] = (peer_rate_t){.used = true, .key = key, .ms = now_ms};
    return true;
}

static peer_rx_t on_invite(peer_session_t *s, uint32_t now_ms, uint16_t link, uint32_t key, peer_frame_t const *f) {
    peer_invite_t inv;
    if (!peer_invite_decode(f->payload, f->len, &inv)) {
        return PEER_RX_BAD;
    }
    if (active(s) && link == s->link) {
        return PEER_RX_IGNORED; // an invite on the session's own link: never drop the session for it
    }
    if (!peer_session_visible(s, now_ms)) {
        return PEER_RX_REFUSE;
    }
    if (s->state == PEER_INVITING && link != s->link) { // glare: the larger nonce wins on both sides
        if (inv.nonce <= s->invite.nonce) {
            return PEER_RX_REFUSE;
        }
        s->yielded_link = s->link;
        take_invite(s, now_ms, link, &inv);
        return PEER_RX_YIELD;
    }
    if (s->state != PEER_IDLE && s->state != PEER_CLOSED) {
        return PEER_RX_REFUSE;
    }
    if (s->state == PEER_CLOSED || !rate_take(s, now_ms, key)) {
        return PEER_RX_REFUSE; // CLOSED: the service has not reset yet
    }
    take_invite(s, now_ms, link, &inv);
    return PEER_RX_OK;
}

// The cumulative ack in every frame of an open session
static void on_ack(peer_session_t *s, uint32_t now_ms, uint16_t ack) {
    uint16_t n = (uint16_t)(ack - s->tx_base);
    if (n == 0 || n > s->tx_ever) {
        return; // nothing new, or an ack for what was never sent
    }
    memmove(&s->txq[0], &s->txq[n], (size_t)(s->txq_n - n) * sizeof(s->txq[0]));
    s->txq_n   = (uint8_t)(s->txq_n - n);
    s->tx_sent = s->tx_sent > n ? (uint8_t)(s->tx_sent - n) : 0;
    s->tx_ever = (uint8_t)(s->tx_ever - n);
    s->tx_base = ack;
    s->retries = 0;
    s->rto_ms  = now_ms;
    if (s->was_busy) {
        s->was_busy  = false;
        s->events   |= PEER_EV_READY;
    }
}

static void on_data(peer_session_t *s, peer_frame_t const *f) {
    s->ack_due = true; // a duplicate or a gap is answered with where we are
    if (f->seq != s->rx_next || s->rxq_n >= PEER_RXQ) {
        return; // full: not acked, so the sender sends it again
    }
    peer_msg_t *m = &s->rxq[(s->rxq_head + s->rxq_n) % PEER_RXQ];
    m->seq        = f->seq;
    m->len        = f->len;
    memcpy(m->data, f->payload, f->len);
    s->rxq_n++;
    s->rx_next++;
    s->events |= PEER_EV_MSG;
}

static peer_rx_t on_session_frame(peer_session_t *s, uint32_t now_ms, peer_frame_t const *f) {
    if (f->type == PEER_F_END) {
        bool exit = f->len == 1 && f->payload[0] == PEER_WHY_APP_EXIT;
        close_as(s, now_ms, exit ? PEER_END_REMOTE_EXIT : PEER_END_REMOTE, 0);
        return PEER_RX_OK;
    }
    if (f->type == PEER_F_DECLINE && (s->state == PEER_INVITING || s->state == PEER_INVITED)) {
        close_as(s, now_ms, PEER_END_DECLINED, 0);
        return PEER_RX_OK;
    }
    if (f->type == PEER_F_ACCEPT && s->state == PEER_INVITING) {
        enter(s, PEER_PAIRING, now_ms);
        s->events |= PEER_EV_PAIR;
        return PEER_RX_OK;
    }
    if (s->state != PEER_OPEN) {
        return PEER_RX_IGNORED; // data before both sides are secured: the retransmission brings it again
    }
    on_ack(s, now_ms, f->ack);
    if (f->type == PEER_F_DATA) {
        on_data(s, f);
    }
    return PEER_RX_OK;
}

peer_rx_t peer_session_on_frame(
    peer_session_t *s, uint32_t now_ms, uint16_t link, uint32_t peer_key, uint8_t const *buf, size_t len
) {
    peer_frame_t f;
    if (!peer_frame_parse(buf, len, &f)) {
        return PEER_RX_BAD;
    }
    if (f.type == PEER_F_INVITE) {
        return on_invite(s, now_ms, link, peer_key, &f);
    }
    if (!active(s) || link != s->link) {
        return PEER_RX_IGNORED;
    }
    s->last_rx_ms = now_ms;
    return on_session_frame(s, now_ms, &f);
}

bool peer_session_answer(peer_session_t *s, uint32_t now_ms, bool accept) {
    if (s->state != PEER_INVITED) {
        return false;
    }
    if (!accept) {
        close_as(s, now_ms, PEER_END_DECLINED, PEER_F_DECLINE);
        return true;
    }
    s->ctl = PEER_F_ACCEPT;
    enter(s, PEER_PAIRING, now_ms);
    return true;
}

void peer_session_secured(peer_session_t *s, uint32_t now_ms, bool ok) {
    if (s->state != PEER_PAIRING) {
        return;
    }
    if (!ok) {
        close_as(s, now_ms, PEER_END_PAIR_FAILED, PEER_F_END);
        return;
    }
    s->tx_base    = 0;
    s->rx_next    = 0;
    s->txq_n      = 0;
    s->tx_sent    = 0;
    s->tx_ever    = 0;
    s->rxq_n      = 0;
    s->retries    = 0;
    s->last_rx_ms = now_ms;
    s->last_tx_ms = now_ms;
    enter(s, PEER_OPEN, now_ms);
    s->events |= PEER_EV_OPEN;
}

void peer_session_link_down(peer_session_t *s, uint32_t now_ms, uint16_t link) {
    if (active(s) && link == s->link) {
        close_as(s, now_ms, PEER_END_LOST, 0);
    }
}

void peer_session_end(peer_session_t *s, uint32_t now_ms) {
    if (active(s)) {
        close_as(s, now_ms, PEER_END_LOCAL, PEER_F_END);
    }
}

void peer_session_app_exit(peer_session_t *s, uint32_t now_ms) {
    if (active(s)) {
        close_as(s, now_ms, PEER_END_APP_EXIT, PEER_F_END);
        s->why = PEER_WHY_APP_EXIT;
    }
}

static void tick_open(peer_session_t *s, uint32_t now_ms) {
    if (since(now_ms, s->last_rx_ms) >= PEER_SILENCE_MS) {
        close_as(s, now_ms, PEER_END_SILENCE, PEER_F_END);
        return;
    }
    if (s->tx_sent == 0 || since(now_ms, s->rto_ms) < PEER_RTO_MS) {
        return;
    }
    if (s->retries >= PEER_RETRIES) {
        close_as(s, now_ms, PEER_END_RETRIES, PEER_F_END);
        return;
    }
    s->retries++;
    s->tx_sent = 0; // go back to the oldest unacked message
}

void peer_session_tick(peer_session_t *s, uint32_t now_ms) {
    uint32_t in_state = since(now_ms, s->state_ms);
    switch (s->state) {
        case PEER_INVITING:
        case PEER_INVITED:
            if (in_state >= PEER_ANSWER_MS) {
                close_as(s, now_ms, PEER_END_NO_ANSWER, PEER_F_END);
            }
            break;
        case PEER_PAIRING:
            if (in_state >= PEER_PAIR_MS) {
                close_as(s, now_ms, PEER_END_PAIR_FAILED, PEER_F_END);
            }
            break;
        case PEER_OPEN: tick_open(s, now_ms); break;
        case PEER_IDLE:
        case PEER_CLOSED: break;
    }
}

lichen_bt_err_t peer_session_send(peer_session_t *s, void const *data, size_t len) {
    if (s->state != PEER_OPEN) {
        return LICHEN_BT_NOT_CONNECTED;
    }
    if (len == 0 || len > PEER_MSG_MAX) {
        return LICHEN_BT_INVALID;
    }
    if (s->txq_n >= PEER_TXQ) {
        s->was_busy = true;
        return LICHEN_BT_BUSY;
    }
    peer_msg_t *m = &s->txq[s->txq_n];
    m->seq        = (uint16_t)(s->tx_base + s->txq_n);
    m->len        = (uint16_t)len;
    memcpy(m->data, data, len);
    s->txq_n++;
    return LICHEN_BT_OK;
}

int peer_session_recv(peer_session_t *s, void *buf, size_t cap) {
    if (s->rxq_n == 0 || cap < s->rxq[s->rxq_head].len) {
        return -1;
    }
    peer_msg_t const *m = &s->rxq[s->rxq_head];
    memcpy(buf, m->data, m->len);
    s->rxq_head = (uint8_t)((s->rxq_head + 1u) % PEER_RXQ);
    s->rxq_n--;
    return m->len;
}

static size_t ctl_frame(peer_session_t *s, uint32_t now_ms, uint8_t *out, size_t cap) {
    uint8_t inv[PEER_INVITE_MAX];
    size_t  plen = 0;
    if (s->ctl == PEER_F_INVITE) {
        plen = peer_invite_encode(&s->invite, inv, sizeof(inv));
    } else if (s->ctl == PEER_F_END && s->why) {
        inv[0] = s->why;
        plen   = 1;
    }
    size_t n = peer_frame_build((peer_ftype_t)s->ctl, 0, s->rx_next, inv, plen, out, cap);
    if (n) {
        s->ctl        = 0;
        s->last_tx_ms = now_ms;
    }
    return n;
}

size_t peer_session_next_frame(peer_session_t *s, uint32_t now_ms, uint8_t *out, size_t cap) {
    if (s->ctl) {
        return ctl_frame(s, now_ms, out, cap);
    }
    if (s->state != PEER_OPEN) {
        return 0;
    }
    size_t n = 0;
    if (s->tx_sent < s->txq_n) {
        peer_msg_t const *m = &s->txq[s->tx_sent];
        n                   = peer_frame_build(PEER_F_DATA, m->seq, s->rx_next, m->data, m->len, out, cap);
        if (n && s->tx_sent++ == 0) {
            s->rto_ms = now_ms; // the round's timer starts with its first frame
        }
        if (s->tx_sent > s->tx_ever) {
            s->tx_ever = s->tx_sent;
        }
    } else if (s->ack_due || since(now_ms, s->last_tx_ms) >= PEER_KEEPALIVE_MS) {
        n = peer_frame_build(PEER_F_ACK, 0, s->rx_next, NULL, 0, out, cap);
    }
    if (n) {
        s->ack_due    = false;
        s->last_tx_ms = now_ms;
    }
    return n;
}

uint8_t peer_session_take_events(peer_session_t *s) {
    uint8_t e = s->events;
    s->events = 0;
    return e;
}

void peer_session_reset(peer_session_t *s) {
    if (s->state != PEER_CLOSED) {
        return;
    }
    bool        visible = s->visible;
    uint32_t    vis_ms  = s->visible_ms;
    peer_rate_t rate[PEER_RATE_SLOTS];
    memcpy(rate, s->rate, sizeof(rate));
    peer_session_init(s);
    s->visible    = visible;
    s->visible_ms = vis_ms;
    memcpy(s->rate, rate, sizeof(rate));
}
