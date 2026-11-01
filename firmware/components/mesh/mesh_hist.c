#include "mesh_hist.h"

#include "mesh_hist_crypto.h"
#include "mesh_outbox.h"

#include <string.h>

// ---- codec ---------------------------------------------------------------------------------------------------------

// The title and channel copies below assume one size for all of them
_Static_assert(sizeof(((mesh_msg_t *)0)->channel) == sizeof(((mesh_hist_known_t *)0)->title), "title sizes");
_Static_assert(sizeof(((mesh_hist_chat_t *)0)->title) == sizeof(((mesh_hist_rec_t *)0)->title), "title sizes");
_Static_assert(sizeof(((mesh_hist_rec_t *)0)->title) == sizeof(((mesh_chat_key_t *)0)->chan), "title sizes");
_Static_assert(sizeof(((mesh_msg_t *)0)->text) == sizeof(((mesh_hist_rec_t *)0)->text), "text sizes");

// CRC-16/CCITT-FALSE (poly 0x1021), a byte at a time: the scan runs it over every byte of the journal at boot
static uint16_t const CRC_TABLE[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0,
};

static uint16_t crc16(uint16_t crc, uint8_t const *p, size_t n) {
    while (n--) {
        crc = (uint16_t)(crc << 8) ^ CRC_TABLE[(uint8_t)(crc >> 8) ^ *p++];
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

// ---- keys (#98) ----------------------------------------------------------------------------------------------------

static char const KEY_LABEL[] = "lichen-journal-v1";

bool mesh_hist_key(uint8_t root[32], uint8_t const *prv, size_t prv_len) {
    return mh_sha256(root, KEY_LABEL, sizeof(KEY_LABEL) - 1, prv, prv_len);
}

void mesh_hist_crypt_init(mesh_hist_crypt_t *c, uint8_t const root[32], void (*rand)(void *buf, size_t len)) {
    memset(c, 0, sizeof(*c));
    memcpy(c->root, root, sizeof(c->root));
    c->have_root = true;
    c->rand      = rand;
}

static bool check_value(mesh_hist_crypt_t const *c, uint8_t const salt[MESH_HIST_SALT], uint8_t out[MESH_HIST_CHECK]) {
    uint8_t mac[32];
    bool    ok = mh_hmac(mac, c->root, sizeof(c->root), "check", 5, salt, MESH_HIST_SALT, NULL, 0);
    memcpy(out, mac, MESH_HIST_CHECK);
    mh_wipe(mac, sizeof(mac));
    return ok;
}

static bool file_keys(mesh_hist_crypt_t *c, uint8_t const salt[MESH_HIST_SALT]) {
    uint8_t k[32];
    memcpy(c->salt, salt, MESH_HIST_SALT);
    bool ok = mh_hmac(k, c->root, sizeof(c->root), "enc", 3, salt, MESH_HIST_SALT, NULL, 0);
    memcpy(c->enc, k, sizeof(c->enc));
    ok = mh_hmac(c->mac, c->root, sizeof(c->root), "mac", 3, salt, MESH_HIST_SALT, NULL, 0) && ok;
    mh_wipe(k, sizeof(k));
    c->open = ok;
    c->error |= !ok;
    return ok;
}

bool mesh_hist_crypt_new(mesh_hist_crypt_t *c) {
    uint8_t salt[MESH_HIST_SALT];
    c->rand(salt, sizeof(salt));
    return file_keys(c, salt);
}

static bool same16(uint8_t const *a, uint8_t const *b) { // constant time
    uint8_t diff = 0;
    for (size_t i = 0; i < 16; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

bool mesh_hist_crypt_open(mesh_hist_crypt_t *c, uint8_t const salt[MESH_HIST_SALT],
                          uint8_t const check[MESH_HIST_CHECK]) {
    uint8_t want[MESH_HIST_CHECK];
    c->open = false;
    if (!c->have_root) {
        return false;
    }
    if (!check_value(c, salt, want)) {
        c->error = true; // the crypto failed: says nothing about the journal
        return false;
    }
    if (!same16(want, check)) {
        return false;
    }
    return file_keys(c, salt);
}

// ---- codec ---------------------------------------------------------------------------------------------------------

// The plaintext header around a payload of plen bytes already at out + MESH_HIST_HDR
static size_t frame(uint8_t *out, uint8_t type, size_t plen, uint32_t seq) {
    out[0] = MESH_HIST_MAGIC;
    out[1] = type;
    put16(out + 2, (uint16_t)plen);
    put32(out + 4, seq);
    uint16_t crc = crc16(0xFFFF, out + 1, 7);
    put16(out + 8, crc16(crc, out + MESH_HIST_HDR, plen));
    return MESH_HIST_HDR + plen;
}

size_t mesh_hist_encode_head(mesh_hist_crypt_t const *c, uint8_t *out, size_t cap) {
    size_t plen = 1 + MESH_HIST_SALT + MESH_HIST_CHECK;
    if (!c || !c->open || cap < MESH_HIST_HDR + plen) {
        return 0;
    }
    out[MESH_HIST_HDR] = MESH_HIST_VERSION;
    memcpy(out + MESH_HIST_HDR + 1, c->salt, MESH_HIST_SALT);
    if (!check_value(c, c->salt, out + MESH_HIST_HDR + 1 + MESH_HIST_SALT)) {
        return 0;
    }
    return frame(out, MESH_HIST_HEAD, plen, 0);
}

// The record's plain payload; 0 when it does not fit in cap or the key is not valid
static size_t plain_payload(mesh_hist_rec_t const *rec, uint8_t *p, size_t cap) {
    wr_t w = {.p = p, .cap = cap};
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
    return w.bad ? 0 : w.n;
}

// The tag over type | seq | nonce | ciphertext (the payload up to the tag)
static bool record_tag(mesh_hist_crypt_t const *c, uint8_t type, uint32_t seq, uint8_t const *p, size_t n,
                       uint8_t tag[MESH_HIST_TAG]) {
    uint8_t ad[5], mac[32];
    ad[0] = type;
    put32(ad + 1, seq);
    bool ok = mh_hmac(mac, c->mac, sizeof(c->mac), ad, sizeof(ad), p, n, NULL, 0);
    memcpy(tag, mac, MESH_HIST_TAG);
    return ok;
}

static void record_iv(uint32_t seq, uint8_t const nonce[MESH_HIST_NONCE], uint8_t iv[12]) {
    put32(iv, seq);
    memcpy(iv + 4, nonce, MESH_HIST_NONCE);
}

size_t mesh_hist_encode(mesh_hist_rec_t const *rec, mesh_hist_crypt_t const *c, uint8_t *out, size_t cap) {
    size_t const over = MESH_HIST_HDR + MESH_HIST_NONCE + MESH_HIST_TAG;
    if (!c || !c->open || cap <= over) {
        return 0;
    }
    size_t   room = (cap < MESH_HIST_REC_MAX ? cap : MESH_HIST_REC_MAX) - over;
    uint8_t *p    = out + MESH_HIST_HDR;
    size_t   n    = plain_payload(rec, p + MESH_HIST_NONCE, room);
    if (n == 0) {
        return 0;
    }
    uint8_t iv[12];
    c->rand(p, MESH_HIST_NONCE);
    record_iv(rec->seq, p, iv);
    // A failed cipher or MAC never lets a record out: plaintext must not reach the card
    if (!mh_aes128_ctr(c->enc, iv, p + MESH_HIST_NONCE, n) ||
        !record_tag(c, rec->type, rec->seq, p, MESH_HIST_NONCE + n, p + MESH_HIST_NONCE + n)) {
        mh_wipe(out, MESH_HIST_HDR + MESH_HIST_NONCE + n + MESH_HIST_TAG);
        return 0;
    }
    return frame(out, rec->type, MESH_HIST_NONCE + n + MESH_HIST_TAG, rec->seq);
}

static mesh_hist_dec_t parse_plain(uint8_t const *p, size_t plen, mesh_hist_rec_t *rec) {
    rd_t r = {.p = p, .len = plen};
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
    default: // READ
        r_key(&r, &rec->key);
        rec->read_seq = r32(&r);
        break;
    }
    return r.bad ? MESH_HIST_AUTH : MESH_HIST_OK; // a good tag over a payload that does not parse: not ours either
}

// Checks the tag, then decrypts into a scratch copy and parses it
static mesh_hist_dec_t decrypt(uint8_t const *p, size_t plen, mesh_hist_crypt_t const *c, mesh_hist_rec_t *rec) {
    if (!c || !c->open || plen < MESH_HIST_NONCE + MESH_HIST_TAG) {
        return MESH_HIST_AUTH;
    }
    size_t  n = plen - MESH_HIST_NONCE - MESH_HIST_TAG;
    uint8_t tag[MESH_HIST_TAG], iv[12], plain[MESH_HIST_REC_MAX];
    if (!record_tag(c, rec->type, rec->seq, p, MESH_HIST_NONCE + n, tag) || !same16(tag, p + MESH_HIST_NONCE + n)) {
        return MESH_HIST_AUTH;
    }
    memcpy(plain, p + MESH_HIST_NONCE, n);
    record_iv(rec->seq, p, iv);
    mesh_hist_dec_t d = mh_aes128_ctr(c->enc, iv, plain, n) ? parse_plain(plain, n, rec) : MESH_HIST_AUTH;
    mh_wipe(plain, n);
    return d;
}

mesh_hist_dec_t mesh_hist_decode(uint8_t const *in, size_t len, mesh_hist_crypt_t const *c, mesh_hist_rec_t *rec,
                                 size_t *used) {
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
    rec->type          = in[1];
    rec->seq           = get32(in + 4);
    *used              = MESH_HIST_HDR + plen;
    uint8_t const *p   = in + MESH_HIST_HDR;
    switch (rec->type) {
    case MESH_HIST_HEAD: {
        rd_t           r    = {.p = p, .len = plen};
        uint8_t        ver  = r8(&r);
        uint8_t const *salt = r_bytes(&r, MESH_HIST_SALT);
        uint8_t const *chk  = r_bytes(&r, MESH_HIST_CHECK);
        if (r.bad || ver != MESH_HIST_VERSION) {
            return r.bad ? MESH_HIST_BAD : MESH_HIST_AUTH; // a newer journal version: unreadable here
        }
        memcpy(rec->salt, salt, MESH_HIST_SALT);
        memcpy(rec->check, chk, MESH_HIST_CHECK);
        return MESH_HIST_OK;
    }
    case MESH_HIST_CHAT:
    case MESH_HIST_MSG:
    case MESH_HIST_READ:
        return decrypt(p, plen, c, rec);
    default:
        return MESH_HIST_OK; // a newer firmware's record: skipped
    }
}

// A bad spot: skip to the next magic byte; a record that decodes there (CRC and all) ends the skip
static void scan_bad(mesh_hist_scan_t *out, uint8_t const *buf, size_t have, size_t *off, uint64_t at, bool *skipping,
                     uint64_t *skip_from) {
    if (!*skipping) {
        *skipping  = true;
        *skip_from = at;
        out->resyncs++;
        if (!out->bad_seen) {
            out->bad_seen  = true;
            out->first_bad = at;
        }
    }
    do {
        (*off)++;
    } while (*off < have && buf[*off] != MESH_HIST_MAGIC);
}

// A whole record (decoded, or failing its tag): false when the scan must stop
static bool scan_good(mesh_hist_scan_t *out, mesh_hist_dec_t d, mesh_hist_rec_t const *rec, mesh_hist_crypt_t *c,
                      uint8_t const *raw, size_t used, mesh_hist_rec_fn on_rec, void *ctx) {
    if (rec->type == MESH_HIST_HEAD && d == MESH_HIST_OK) {
        if (!c || !mesh_hist_crypt_open(c, rec->salt, rec->check)) {
            if (c && c->error) {
                out->crypto_error = true; // not the journal's fault: never moved aside for this
            } else {
                out->unreadable = true; // another node's journal, or one from before a new identity
            }
            return false;
        }
        out->head = true;
    } else if (!out->head) {
        out->unreadable = true; // no HEAD first: a plaintext journal of an older build, or not ours
        return false;
    }
    if (d == MESH_HIST_AUTH) {
        out->auth_failures++;
        return true;
    }
    if (on_rec && !on_rec(rec, raw, used, ctx)) {
        out->stopped = true;
        return false;
    }
    return true;
}

void mesh_hist_scan(mesh_hist_read_fn read, void *rctx, uint8_t *buf, mesh_hist_crypt_t *c, mesh_hist_rec_fn on_rec,
                    void *ctx, mesh_hist_scan_t *out) {
    mesh_hist_rec_t rec;
    size_t          have = 0, off = 0;
    uint64_t        base = 0, skip_from = 0; // base: the file offset of buf[0]
    bool            skipping = false;
    memset(out, 0, sizeof(*out));
    for (;;) {
        size_t          used = 0;
        mesh_hist_dec_t d    = mesh_hist_decode(buf + off, have - off, c, &rec, &used);
        if (d == MESH_HIST_OK || d == MESH_HIST_AUTH) {
            if (skipping) {
                skipping = false;
                out->skipped_bytes += base + off - skip_from;
            }
            uint8_t const *raw = buf + off;
            off += used;
            out->good_bytes = base + off;
            out->records++;
            if (!scan_good(out, d, &rec, c, raw, used, on_rec, ctx)) {
                return;
            }
            continue;
        }
        if (d == MESH_HIST_BAD) {
            scan_bad(out, buf, have, &off, base + off, &skipping, &skip_from);
            if (base + off - skip_from > MESH_HIST_RESYNC_MAX) {
                out->torn = true; // no record within reach: the rest is not trusted
                return;
            }
            continue;
        }
        // Less than one record left (under MESH_HIST_REC_MAX bytes): to the front, then the next chunk behind it
        memmove(buf, buf + off, have - off);
        base += off;
        have -= off;
        off = 0;
        size_t got = read(rctx, buf + have, MESH_HIST_CHUNK);
        if (got == 0) {
            out->torn = have > 0 || skipping; // cut inside a record, or garbage up to the end
            if (skipping) {
                out->skipped_bytes += base + have - skip_from;
            }
            return;
        }
        have += got;
    }
}

// ---- the writer ----------------------------------------------------------------------------------------------------

void mesh_hist_init(mesh_hist_t *h, uint8_t *buf, size_t cap, mesh_hist_crypt_t const *crypt) {
    memset(h, 0, sizeof(*h));
    h->buf   = buf;
    h->cap   = cap;
    h->crypt = crypt;
}

static bool emit(mesh_hist_t *h, mesh_hist_rec_t const *rec, uint32_t now_s) {
    uint8_t tmp[MESH_HIST_REC_MAX];
    size_t  n = mesh_hist_encode(rec, h->crypt, tmp, sizeof(tmp));
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

void mesh_hist_forget_chats(mesh_hist_t *h) {
    memset(h->known, 0, sizeof(h->known));
    h->known_next = 0;
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

// The message's index among its chat's messages: a rewritten outgoing message (same seq, a newer state) gets the index
// of its first copy, so a chat keeps keep messages, not keep records. *dup: it is such a copy.
static uint32_t msg_index(mesh_hist_chat_t *c, mesh_hist_rec_t const *r, uint32_t *next, bool *dup) {
    *dup = false;
    if (!r->outgoing) {
        return (*next)++;
    }
    for (size_t i = 0; i < MESH_HIST_DUP; i++) {
        if (c->out_used[i] && c->out_seq[i] == r->seq) {
            *dup = true;
            return c->out_index[i];
        }
    }
    size_t at        = c->out_next;
    c->out_next      = (uint8_t)((c->out_next + 1) % MESH_HIST_DUP);
    c->out_used[at]  = true;
    c->out_seq[at]   = r->seq;
    c->out_index[at] = *next;
    return (*next)++;
}

static void forget_outgoing(mesh_hist_chat_t *c) {
    memset(c->out_used, 0, sizeof(c->out_used));
    c->out_next = 0;
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
        return;
    }
    bool dup;
    msg_index(c, r, &c->msgs, &dup);
    if (!dup) {
        c->msg_bytes += bytes;
    }
}

static size_t chat_rec_bytes(mesh_hist_chat_t const *c) {
    size_t key = c->key.kind == MESH_CHAT_DM ? 1 + 6 : 2 + strlen(c->key.chan);
    return MESH_HIST_HDR + MESH_HIST_NONCE + MESH_HIST_TAG + key + 1 + strlen(c->title);
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
    mesh_hist_crypt_t   *crypt;
    mesh_hist_table_t   *t;
    size_t               keep;
    bool                 head_written;
    mesh_hist_write_fn   write;
    void                *wctx;
    uint8_t             *out;
    size_t               out_cap, out_len;
    mesh_hist_compact_t *st;
} compact_ctx_t;

// Room for one more record in the staging buffer
static bool stage_room(compact_ctx_t *c) {
    if (c->out_cap - c->out_len < MESH_HIST_REC_MAX) {
        if (!c->write(c->wctx, c->out, c->out_len)) {
            c->st->ok = false;
            return false;
        }
        c->out_len = 0;
    }
    return true;
}

static void staged(compact_ctx_t *c, size_t n) {
    c->out_len += n;
    c->st->bytes_out += n;
    c->st->records_out += n ? 1 : 0;
}

// A record as it is in the journal (same nonce, same ciphertext)
static bool stage_raw(compact_ctx_t *c, uint8_t const *raw, size_t n) {
    if (!stage_room(c)) {
        return false;
    }
    memcpy(c->out + c->out_len, raw, n);
    staged(c, n);
    return true;
}

// A new record (a chat's CHAT with its last title), encrypted with the journal's keys and a fresh nonce
static bool stage(compact_ctx_t *c, mesh_hist_rec_t const *rec) {
    if (!stage_room(c)) {
        return false;
    }
    staged(c, mesh_hist_encode(rec, c->crypt, c->out + c->out_len, c->out_cap - c->out_len));
    return true;
}

static bool compact_rec(mesh_hist_rec_t const *r, uint8_t const *raw, size_t bytes, void *ctx) {
    compact_ctx_t *c = (compact_ctx_t *)ctx;
    if (r->type == MESH_HIST_HEAD) {
        bool first      = !c->head_written;
        c->head_written = true;
        return !first || stage_raw(c, raw, bytes); // the same salt: the writer's keys stay valid
    }
    if (r->type != MESH_HIST_CHAT && r->type != MESH_HIST_MSG) {
        return true; // READ records go
    }
    mesh_hist_chat_t *slot = table_slot(c->t, &r->key, false);
    if (!slot) {
        return stage_raw(c, raw, bytes); // a chat the table had no room for: all of its records stay
    }
    if (r->type == MESH_HIST_CHAT) {
        return true; // written once, before its first kept message, with its last title
    }
    bool     dup;
    uint32_t index = msg_index(slot, r, &slot->seen, &dup);
    if (slot->msgs > c->keep && index < slot->msgs - c->keep) {
        return true;
    }
    if (!slot->written) {
        mesh_hist_rec_t chat;
        memset(&chat, 0, sizeof(chat));
        chat.type = MESH_HIST_CHAT;
        chat.seq  = r->seq; // a seq of its own for the nonce's counter block, next to the random nonce
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
    return stage_raw(c, raw, bytes);
}

void mesh_hist_compact(mesh_hist_read_fn read, void *rctx, uint8_t *buf, mesh_hist_crypt_t *crypt, mesh_hist_table_t *t,
                       size_t keep, mesh_hist_write_fn write, void *wctx, uint8_t *out, size_t out_cap,
                       mesh_hist_compact_t *st) {
    memset(st, 0, sizeof(*st));
    st->ok = out_cap >= MESH_HIST_REC_MAX;
    if (!st->ok) {
        return;
    }
    for (size_t i = 0; i < t->cap; i++) {
        t->slots[i].seen    = 0;
        t->slots[i].written = false;
        forget_outgoing(&t->slots[i]);
    }
    compact_ctx_t c = {.crypt   = crypt,
                       .t       = t,
                       .keep    = keep,
                       .write   = write,
                       .wctx    = wctx,
                       .out     = out,
                       .out_cap = out_cap,
                       .st      = st};
    mesh_hist_scan_t scan;
    mesh_hist_scan(read, rctx, buf, crypt, compact_rec, &c, &scan);
    st->auth_failures = scan.auth_failures;
    if (!scan.head) {
        st->ok = false; // never write a journal without its HEAD
    }
    if (st->ok && c.out_len && !write(wctx, out, c.out_len)) {
        st->ok = false;
    }
}
