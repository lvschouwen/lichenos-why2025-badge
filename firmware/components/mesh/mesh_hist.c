#include "mesh_hist.h"

#include "mesh_outbox.h"

#include <string.h>

// ---- codec ---------------------------------------------------------------------------------------------------------

static uint16_t crc16(uint16_t crc, uint8_t const *p, size_t n) { // CRC-16/CCITT-FALSE
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) {
            crc = (uint16_t)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
        }
    }
    return crc;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}
static uint16_t get16(uint8_t const *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}
static uint32_t get32(uint8_t const *p) {
    return get16(p) | (uint32_t)get16(p + 2) << 16;
}

// A bounded writer and reader over a payload: once out of room (or input) they stay failed
typedef struct {
    uint8_t *p;
    size_t   n, cap;
    bool     bad;
} wr_t;
typedef struct {
    uint8_t const *p;
    size_t         n, len;
    bool           bad;
} rd_t;

static void w_bytes(wr_t *w, void const *src, size_t n) {
    if (w->bad || w->n + n > w->cap) {
        w->bad = true;
        return;
    }
    memcpy(w->p + w->n, src, n);
    w->n += n;
}
static void w8(wr_t *w, uint8_t v) {
    w_bytes(w, &v, 1);
}
static void w16(wr_t *w, uint16_t v) {
    uint8_t b[2];
    put16(b, v);
    w_bytes(w, b, 2);
}
static void w32(wr_t *w, uint32_t v) {
    uint8_t b[4];
    put32(b, v);
    w_bytes(w, b, 4);
}
static void w_str(wr_t *w, char const *s, size_t max) { // length byte, then the bytes
    size_t n = strnlen(s, max);
    w8(w, (uint8_t)n);
    w_bytes(w, s, n);
}

static uint8_t const *r_bytes(rd_t *r, size_t n) {
    if (r->bad || r->n + n > r->len) {
        r->bad = true;
        return NULL;
    }
    uint8_t const *at = r->p + r->n;
    r->n += n;
    return at;
}
static uint8_t r8(rd_t *r) {
    uint8_t const *b = r_bytes(r, 1);
    return b ? b[0] : 0;
}
static uint16_t r16(rd_t *r) {
    uint8_t const *b = r_bytes(r, 2);
    return b ? get16(b) : 0;
}
static uint32_t r32(rd_t *r) {
    uint8_t const *b = r_bytes(r, 4);
    return b ? get32(b) : 0;
}
static void r_str(rd_t *r, char *out, size_t max) { // max: bytes the field holds without its NUL
    size_t n = r8(r);
    if (n > max) {
        r->bad = true;
        return;
    }
    uint8_t const *b = r_bytes(r, n);
    if (b) {
        memcpy(out, b, n);
        out[n] = '\0';
    }
}

static void w_key(wr_t *w, mesh_chat_key_t const *k) {
    w8(w, k->kind);
    if (k->kind == MESH_CHAT_DM) {
        w_bytes(w, k->id, sizeof(k->id));
    } else if (k->kind == MESH_CHAT_CHANNEL) {
        w_str(w, k->chan, MESH_CHANNEL_MAX);
    } else {
        w->bad = true;
    }
}
static void r_key(rd_t *r, mesh_chat_key_t *k) {
    memset(k, 0, sizeof(*k));
    k->kind = r8(r);
    if (k->kind == MESH_CHAT_DM) {
        uint8_t const *b = r_bytes(r, sizeof(k->id));
        if (b) {
            memcpy(k->id, b, sizeof(k->id));
        }
    } else if (k->kind == MESH_CHAT_CHANNEL) {
        r_str(r, k->chan, MESH_CHANNEL_MAX);
    } else {
        r->bad = true;
    }
}

size_t mesh_hist_encode(mesh_hist_rec_t const *rec, uint8_t *out, size_t cap) {
    if (cap < MESH_HIST_HDR) {
        return 0;
    }
    size_t room = cap - MESH_HIST_HDR < MESH_HIST_REC_MAX - MESH_HIST_HDR ? cap - MESH_HIST_HDR
                                                                         : MESH_HIST_REC_MAX - MESH_HIST_HDR;
    wr_t w = {.p = out + MESH_HIST_HDR, .cap = room};
    w_key(&w, &rec->key);
    switch (rec->type) {
    case MESH_HIST_CHAT:
        w_str(&w, rec->title, MESH_CHANNEL_MAX);
        break;
    case MESH_HIST_MSG:
        w32(&w, rec->epoch);
        w16(&w, (uint16_t)rec->rssi_x10);
        w8(&w, rec->outgoing ? 1 : 0);
        w8(&w, rec->state);
        w8(&w, rec->repeats);
        w32(&w, rec->trip_ms);
        w_str(&w, rec->text, MESH_TEXT_MAX);
        break;
    case MESH_HIST_READ:
        w32(&w, rec->read_seq);
        break;
    default:
        return 0;
    }
    if (w.bad) {
        return 0;
    }
    out[0] = MESH_HIST_MAGIC;
    out[1] = rec->type;
    put16(out + 2, (uint16_t)w.n);
    put32(out + 4, rec->seq);
    uint16_t crc = crc16(0xFFFF, out + 1, 7);
    put16(out + 8, crc16(crc, out + MESH_HIST_HDR, w.n));
    return MESH_HIST_HDR + w.n;
}

mesh_hist_dec_t mesh_hist_decode(uint8_t const *in, size_t len, mesh_hist_rec_t *rec, size_t *used) {
    if (len == 0) {
        return MESH_HIST_MORE;
    }
    if (in[0] != MESH_HIST_MAGIC) {
        return MESH_HIST_BAD;
    }
    if (len < MESH_HIST_HDR) {
        return MESH_HIST_MORE;
    }
    size_t plen = get16(in + 2);
    if (plen > MESH_HIST_REC_MAX - MESH_HIST_HDR) {
        return MESH_HIST_BAD;
    }
    if (len < MESH_HIST_HDR + plen) {
        return MESH_HIST_MORE;
    }
    uint16_t crc = crc16(0xFFFF, in + 1, 7);
    if (crc16(crc, in + MESH_HIST_HDR, plen) != get16(in + 8)) {
        return MESH_HIST_BAD;
    }
    memset(rec, 0, sizeof(*rec));
    rec->type = in[1];
    rec->seq  = get32(in + 4);
    *used     = MESH_HIST_HDR + plen;
    rd_t r    = {.p = in + MESH_HIST_HDR, .len = plen};
    switch (rec->type) {
    case MESH_HIST_CHAT:
        r_key(&r, &rec->key);
        r_str(&r, rec->title, MESH_CHANNEL_MAX);
        break;
    case MESH_HIST_MSG:
        r_key(&r, &rec->key);
        rec->epoch    = r32(&r);
        rec->rssi_x10 = (int16_t)r16(&r);
        rec->outgoing = r8(&r) & 1;
        rec->state    = r8(&r);
        rec->repeats  = r8(&r);
        rec->trip_ms  = r32(&r);
        r_str(&r, rec->text, MESH_TEXT_MAX);
        break;
    case MESH_HIST_READ:
        r_key(&r, &rec->key);
        rec->read_seq = r32(&r);
        break;
    default:
        return MESH_HIST_OK; // a newer firmware's record: skipped
    }
    return r.bad ? MESH_HIST_BAD : MESH_HIST_OK;
}

void mesh_hist_scan(mesh_hist_read_fn read, void *rctx, uint8_t *buf, mesh_hist_rec_fn on_rec, void *ctx,
                    mesh_hist_scan_t *out) {
    mesh_hist_rec_t rec;
    size_t          have = 0, off = 0;
    memset(out, 0, sizeof(*out));
    for (;;) {
        size_t          used = 0;
        mesh_hist_dec_t d    = mesh_hist_decode(buf + off, have - off, &rec, &used);
        if (d == MESH_HIST_OK) {
            off += used;
            out->good_bytes += used;
            out->records++;
            if (on_rec && !on_rec(&rec, used, ctx)) {
                out->stopped = true;
                return;
            }
            continue;
        }
        if (d == MESH_HIST_BAD) {
            out->torn = true;
            return;
        }
        // Less than one record left (under MESH_HIST_REC_MAX bytes): to the front, then the next chunk behind it
        memmove(buf, buf + off, have - off);
        have -= off;
        off = 0;
        size_t got = read(rctx, buf + have, MESH_HIST_CHUNK);
        if (got == 0) {
            out->torn = have > 0;
            return;
        }
        have += got;
    }
}

// ---- the writer ----------------------------------------------------------------------------------------------------

void mesh_hist_init(mesh_hist_t *h, uint8_t *buf, size_t cap) {
    memset(h, 0, sizeof(*h));
    h->buf = buf;
    h->cap = cap;
}

static bool emit(mesh_hist_t *h, mesh_hist_rec_t const *rec, uint32_t now_s) {
    uint8_t tmp[MESH_HIST_REC_MAX];
    size_t  n = mesh_hist_encode(rec, tmp, sizeof(tmp));
    if (n == 0 || h->len + n > h->cap) {
        h->dropped++;
        return false;
    }
    if (h->len == 0) {
        h->oldest_s = now_s;
    }
    memcpy(h->buf + h->len, tmp, n);
    h->len += n;
    h->pending_records++;
    return true;
}

static bool key_same(mesh_chat_key_t const *a, mesh_chat_key_t const *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

static mesh_hist_known_t *known_find(mesh_hist_t *h, mesh_chat_key_t const *key) {
    for (size_t i = 0; i < MESH_HIST_KNOWN; i++) {
        if (h->known[i].used && key_same(&h->known[i].key, key)) {
            return &h->known[i];
        }
    }
    return NULL;
}

static void known_set(mesh_hist_t *h, mesh_chat_key_t const *key, char const *title) {
    mesh_hist_known_t *k = known_find(h, key);
    if (!k) { // the table is a cache: an evicted chat only gets its CHAT record once more
        k = &h->known[h->known_next];
        h->known_next = (h->known_next + 1) % MESH_HIST_KNOWN;
        k->used = true;
        k->key  = *key;
    }
    mesh_sanitize(title, strlen(title), k->title, sizeof(k->title));
}

// The chat's CHAT record unless the journal has it with this title (an unknown contact never replaces a name)
static bool need_chat(mesh_hist_t *h, mesh_chat_key_t const *key, char const *title, uint32_t now_s) {
    mesh_hist_known_t const *k = known_find(h, key);
    if (k && (strcmp(k->title, title) == 0 || strcmp(title, "@?") == 0)) {
        return true;
    }
    mesh_hist_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.type = MESH_HIST_CHAT;
    rec.key  = *key;
    mesh_sanitize(title, strlen(title), rec.title, sizeof(rec.title));
    if (!emit(h, &rec, now_s)) {
        return false;
    }
    known_set(h, key, title);
    return true;
}

static bool write_msg(mesh_hist_t *h, mesh_chat_key_t const *key, mesh_msg_t const *m, uint32_t now_s) {
    if (!need_chat(h, key, m->channel, now_s)) {
        return false;
    }
    mesh_hist_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.type     = MESH_HIST_MSG;
    rec.seq      = m->seq;
    rec.key      = *key;
    rec.epoch    = m->epoch;
    rec.rssi_x10 = m->rssi_x10;
    rec.outgoing = m->outgoing;
    rec.state    = m->state;
    rec.repeats  = m->repeats;
    rec.trip_ms  = m->trip_ms;
    memcpy(rec.text, m->text, sizeof(rec.text));
    return emit(h, &rec, now_s);
}

bool mesh_hist_add(mesh_hist_t *h, mesh_chat_key_t const *key, mesh_msg_t const *m, uint32_t now_s) {
    return write_msg(h, key, m, now_s);
}

bool mesh_hist_out_final(mesh_msg_t const *m) {
    switch ((mesh_out_state_t)m->state) {
    case MESH_OUT_DELIVERED:
    case MESH_OUT_FAILED:
        return true;
    case MESH_OUT_ON_AIR:
    case MESH_OUT_HEARD:
        return !m->dm; // a DM is still waiting for its ACK
    default:
        return false;
    }
}

// Writes a held message when its state is final and the journal does not have that state yet
static void held_write(mesh_hist_t *h, mesh_hist_held_t *e, uint32_t now_s) {
    mesh_msg_t const *m = &e->msg;
    if (!mesh_hist_out_final(m) ||
        (e->written && e->w_state == m->state && e->w_repeats == m->repeats && e->w_trip_ms == m->trip_ms)) {
        return;
    }
    if (write_msg(h, &e->key, m, now_s)) {
        e->written   = true;
        e->w_state   = m->state;
        e->w_repeats = m->repeats;
        e->w_trip_ms = m->trip_ms;
    }
}

// The message leaves the table: written as it stands, "not sent" when it never got to a final state
static void held_settle(mesh_hist_t *h, mesh_hist_held_t *e, uint32_t now_s) {
    if (!mesh_hist_out_final(&e->msg)) {
        e->msg.state = MESH_OUT_FAILED;
    }
    held_write(h, e, now_s);
    e->used = false;
}

void mesh_hist_out_new(mesh_hist_t *h, mesh_chat_key_t const *key, mesh_msg_t const *m, uint32_t now_s) {
    mesh_hist_held_t *slot = NULL;
    for (size_t i = 0; i < MESH_HIST_HOLD && !(slot && !slot->used); i++) {
        mesh_hist_held_t *e = &h->held[i];
        if (!e->used || !slot || e->msg.seq < slot->msg.seq) {
            slot = e;
        }
    }
    if (slot->used) {
        held_settle(h, slot, now_s); // full: the oldest goes (the outbox has dropped it by now)
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->key  = *key;
    slot->msg  = *m;
    held_write(h, slot, now_s);
}

void mesh_hist_out_state(mesh_hist_t *h, uint32_t id, uint8_t state, uint8_t repeats, uint8_t attempt,
                         uint32_t trip_ms, uint32_t now_s) {
    for (size_t i = 0; id && i < MESH_HIST_HOLD; i++) {
        mesh_hist_held_t *e = &h->held[i];
        if (e->used && e->msg.id == id) {
            e->msg.state   = state;
            e->msg.repeats = repeats;
            e->msg.attempt = attempt;
            e->msg.trip_ms = trip_ms;
            held_write(h, e, now_s);
            return;
        }
    }
}

void mesh_hist_out_sweep(mesh_hist_t *h, uint32_t const *ids, size_t n, uint32_t now_s) {
    for (size_t i = 0; i < MESH_HIST_HOLD; i++) {
        mesh_hist_held_t *e = &h->held[i];
        bool              in_outbox = false;
        for (size_t k = 0; e->used && k < n && !in_outbox; k++) {
            in_outbox = ids[k] == e->msg.id;
        }
        if (e->used && !in_outbox) {
            held_settle(h, e, now_s);
        }
    }
}

void mesh_hist_stop(mesh_hist_t *h, uint32_t now_s) {
    for (size_t i = 0; i < MESH_HIST_HOLD; i++) {
        if (h->held[i].used) {
            held_settle(h, &h->held[i], now_s);
        }
    }
}

size_t mesh_hist_held_count(mesh_hist_t const *h) {
    size_t n = 0;
    for (size_t i = 0; i < MESH_HIST_HOLD; i++) {
        n += h->held[i].used;
    }
    return n;
}

bool mesh_hist_should_flush(uint32_t oldest_age_s, size_t fill, bool stopping) {
    if (fill == 0) {
        return false;
    }
    return stopping || oldest_age_s >= MESH_HIST_FLUSH_S || fill >= MESH_HIST_PENDING / 4 * 3;
}

uint8_t *mesh_hist_swap(mesh_hist_t *h, uint8_t *fresh, size_t *len, uint32_t *records) {
    uint8_t *old       = h->buf;
    *len               = h->len;
    *records           = h->pending_records;
    h->buf             = fresh;
    h->len             = 0;
    h->pending_records = 0;
    return old;
}

bool mesh_hist_replay(mesh_hist_t *h, mesh_hist_rec_t const *r) {
    if (r->type == MESH_HIST_CHAT) {
        known_set(h, &r->key, r->title);
        return true;
    }
    if (r->type != MESH_HIST_MSG) {
        return true; // READ: loaded messages count as read anyway; unknown types are skipped
    }
    mesh_hist_known_t const *k = known_find(h, &r->key);
    mesh_msg_t               m;
    memset(&m, 0, sizeof(m));
    if (k) {
        memcpy(m.channel, k->title, sizeof(m.channel));
    } else if (r->key.kind == MESH_CHAT_CHANNEL) {
        memcpy(m.channel, r->key.chan, sizeof(m.channel));
    } else {
        strcpy(m.channel, "@?");
    }
    memcpy(m.text, r->text, sizeof(m.text));
    m.rssi_x10 = r->rssi_x10;
    m.outgoing = r->outgoing;
    m.state    = r->state;
    m.repeats  = r->repeats;
    m.trip_ms  = r->trip_ms;
    m.seq      = r->seq;
    m.epoch    = r->epoch;
    if (r->key.kind == MESH_CHAT_DM) {
        m.dm = 1;
        memcpy(m.peer, r->key.id, sizeof(m.peer));
    }
    if (!mesh_store_add_loaded(&r->key, m.channel, &m)) {
        return false;
    }
    h->loaded++;
    return true;
}

// ---- compaction ----------------------------------------------------------------------------------------------------

void mesh_hist_table_init(mesh_hist_table_t *t, mesh_hist_chat_t *slots, size_t cap) {
    memset(slots, 0, cap * sizeof(*slots));
    t->slots    = slots;
    t->cap      = cap;
    t->n        = 0;
    t->overflow = false;
}

static uint32_t key_hash(mesh_chat_key_t const *k) { // FNV-1a
    uint8_t const *p = (uint8_t const *)k;
    uint32_t       h = 2166136261u;
    for (size_t i = 0; i < sizeof(*k); i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

// The chat's slot (open addressing); a new one with add, NULL when it is not there (or the table is full)
static mesh_hist_chat_t *table_slot(mesh_hist_table_t *t, mesh_chat_key_t const *key, bool add) {
    size_t mask = t->cap - 1;
    for (size_t i = key_hash(key) & mask, probes = 0; probes < t->cap; i = (i + 1) & mask, probes++) {
        mesh_hist_chat_t *c = &t->slots[i];
        if (c->used && key_same(&c->key, key)) {
            return c;
        }
        if (!c->used) {
            if (!add || t->n + 1 >= t->cap) { // one slot stays free so a lookup always ends
                t->overflow |= add;
                return NULL;
            }
            c->used = true;
            c->key  = *key;
            t->n++;
            return c;
        }
    }
    return NULL;
}

void mesh_hist_count(mesh_hist_table_t *t, mesh_hist_rec_t const *r, size_t bytes) {
    if (r->type != MESH_HIST_CHAT && r->type != MESH_HIST_MSG) {
        return;
    }
    mesh_hist_chat_t *c = table_slot(t, &r->key, true);
    if (!c) {
        return;
    }
    if (r->type == MESH_HIST_CHAT) {
        memcpy(c->title, r->title, sizeof(c->title));
    } else {
        c->msgs++;
        c->msg_bytes += bytes;
    }
}

static size_t chat_rec_bytes(mesh_hist_chat_t const *c) {
    size_t key = c->key.kind == MESH_CHAT_DM ? 1 + 6 : 2 + strlen(c->key.chan);
    return MESH_HIST_HDR + key + 1 + strlen(c->title);
}

uint64_t mesh_hist_live_bytes(mesh_hist_table_t const *t, size_t keep) {
    uint64_t live = 0;
    for (size_t i = 0; i < t->cap; i++) {
        mesh_hist_chat_t const *c = &t->slots[i];
        if (c->used && c->msgs) {
            live += chat_rec_bytes(c) + (c->msgs <= keep ? c->msg_bytes : c->msg_bytes * keep / c->msgs);
        }
    }
    return live;
}

bool mesh_hist_should_compact(uint64_t file_bytes, uint64_t live_bytes, bool torn) {
    return torn || file_bytes > MESH_HIST_CAP || (file_bytes > MESH_HIST_COMPACT_MIN && file_bytes > 2 * live_bytes);
}

bool mesh_hist_space_ok(uint64_t free_bytes, uint64_t log_bytes) {
    uint64_t grow = log_bytes < MESH_HIST_CAP ? MESH_HIST_CAP - log_bytes : 0;
    return free_bytes >= grow + MESH_HIST_RESERVE;
}

typedef struct {
    mesh_hist_table_t   *t;
    size_t               keep;
    mesh_hist_write_fn   write;
    void                *wctx;
    uint8_t             *out;
    size_t               out_cap, out_len;
    mesh_hist_compact_t *st;
} compact_ctx_t;

static bool stage(compact_ctx_t *c, mesh_hist_rec_t const *rec) {
    if (c->out_cap - c->out_len < MESH_HIST_REC_MAX) {
        if (!c->write(c->wctx, c->out, c->out_len)) {
            c->st->ok = false;
            return false;
        }
        c->out_len = 0;
    }
    size_t n = mesh_hist_encode(rec, c->out + c->out_len, c->out_cap - c->out_len);
    c->out_len += n;
    c->st->bytes_out += n;
    c->st->records_out += n ? 1 : 0;
    return true;
}

static bool compact_rec(mesh_hist_rec_t const *r, size_t bytes, void *ctx) {
    compact_ctx_t *c = (compact_ctx_t *)ctx;
    (void)bytes;
    if (r->type != MESH_HIST_CHAT && r->type != MESH_HIST_MSG) {
        return true; // READ records go
    }
    mesh_hist_chat_t *slot = table_slot(c->t, &r->key, false);
    if (!slot) {
        return stage(c, r); // a chat the table had no room for: all of its records stay
    }
    if (r->type == MESH_HIST_CHAT) {
        return true; // written once, before its first kept message, with its last title
    }
    uint32_t index = slot->seen++;
    if (slot->msgs > c->keep && index < slot->msgs - c->keep) {
        return true;
    }
    if (!slot->written) {
        mesh_hist_rec_t chat;
        memset(&chat, 0, sizeof(chat));
        chat.type = MESH_HIST_CHAT;
        chat.key  = slot->key;
        memcpy(chat.title, slot->title[0] ? slot->title : slot->key.chan, sizeof(chat.title));
        if (!chat.title[0]) {
            strcpy(chat.title, "@?");
        }
        if (!stage(c, &chat)) {
            return false;
        }
        slot->written = true;
    }
    return stage(c, r);
}

void mesh_hist_compact(mesh_hist_read_fn read, void *rctx, uint8_t *buf, mesh_hist_table_t *t, size_t keep,
                       mesh_hist_write_fn write, void *wctx, uint8_t *out, size_t out_cap, mesh_hist_compact_t *st) {
    memset(st, 0, sizeof(*st));
    st->ok = out_cap >= MESH_HIST_REC_MAX;
    if (!st->ok) {
        return;
    }
    for (size_t i = 0; i < t->cap; i++) {
        t->slots[i].seen    = 0;
        t->slots[i].written = false;
    }
    compact_ctx_t    c = {.t = t, .keep = keep, .write = write, .wctx = wctx, .out = out, .out_cap = out_cap, .st = st};
    mesh_hist_scan_t scan;
    mesh_hist_scan(read, rctx, buf, compact_rec, &c, &scan);
    if (st->ok && c.out_len && !write(wctx, out, c.out_len)) {
        st->ok = false;
    }
}
