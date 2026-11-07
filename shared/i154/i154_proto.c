// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
#include "i154_proto.h"

#include <string.h>

// ---- messages ----

#define HDR_LEN        2
#define CAPS_REQ_LEN   HDR_LEN
#define SCAN_START_LEN (HDR_LEN + 8)
#define SCAN_STOP_LEN  (HDR_LEN + 1)
#define CAPS_LEN       (HDR_LEN + 6)
#define ED_LEN         (HDR_LEN + 3)
#define FRAME_LEN      (HDR_LEN + 32 + I154_NAME_LEN)
#define DONE_LEN       (HDR_LEN + 11)

_Static_assert(FRAME_LEN <= I154_MSG_MAX, "FRAME must fit I154_MSG_MAX");

static uint8_t *put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return p + 2;
}

static uint8_t *put_u32(uint8_t *p, uint32_t v) {
    p = put_u16(p, (uint16_t)v);
    return put_u16(p, (uint16_t)(v >> 16));
}

static uint8_t *put_u64(uint8_t *p, uint64_t v) {
    p = put_u32(p, (uint32_t)v);
    return put_u32(p, (uint32_t)(v >> 32));
}

static uint16_t get_u16(uint8_t const **p) {
    uint16_t v  = (uint16_t)((*p)[0] | (*p)[1] << 8);
    *p         += 2;
    return v;
}

static uint32_t get_u32(uint8_t const **p) {
    uint32_t lo = get_u16(p);
    return lo | (uint32_t)get_u16(p) << 16;
}

static uint64_t get_u64(uint8_t const **p) {
    uint64_t lo = get_u32(p);
    return lo | (uint64_t)get_u32(p) << 32;
}

static void enc_none(i154_msg_t const *m, uint8_t *p) {
    (void)m;
    (void)p;
}

static void dec_none(uint8_t const *p, i154_msg_t *m) {
    (void)m;
    (void)p;
}

static void enc_scan(i154_msg_t const *m, uint8_t *p) {
    i154_scan_req_t const *s = &m->u.scan;
    *p++                     = s->seq;
    *p++                     = s->mode;
    p                        = put_u32(p, s->channel_mask);
    put_u16(p, s->dwell_ms);
}

static void dec_scan(uint8_t const *p, i154_msg_t *m) {
    i154_scan_req_t *s = &m->u.scan;
    s->seq             = *p++;
    s->mode            = *p++;
    s->channel_mask    = get_u32(&p);
    s->dwell_ms        = get_u16(&p);
}

static void enc_stop(i154_msg_t const *m, uint8_t *p) {
    *p = m->u.stop_seq;
}

static void dec_stop(uint8_t const *p, i154_msg_t *m) {
    m->u.stop_seq = *p;
}

static void enc_caps(i154_msg_t const *m, uint8_t *p) {
    *p++ = m->u.caps.modes;
    p    = put_u32(p, m->u.caps.channel_mask);
    *p   = m->u.caps.table_size;
}

static void dec_caps(uint8_t const *p, i154_msg_t *m) {
    m->u.caps.modes        = *p++;
    m->u.caps.channel_mask = get_u32(&p);
    m->u.caps.table_size   = *p;
}

static void enc_ed(i154_msg_t const *m, uint8_t *p) {
    p[0] = m->u.ed.seq;
    p[1] = m->u.ed.channel;
    p[2] = (uint8_t)m->u.ed.level_dbm;
}

static void dec_ed(uint8_t const *p, i154_msg_t *m) {
    m->u.ed.seq       = p[0];
    m->u.ed.channel   = p[1];
    m->u.ed.level_dbm = (int8_t)p[2];
}

static void enc_frame(i154_msg_t const *m, uint8_t *p) {
    i154_frame_sum_t const *f = &m->u.frame;
    *p++                      = f->seq;
    *p++                      = f->channel;
    p                         = put_u16(p, f->pan_id);
    *p++                      = (uint8_t)f->rssi_last;
    *p++                      = (uint8_t)f->rssi_best;
    *p++                      = f->lqi_last;
    p                         = put_u16(p, f->frames);
    p                         = put_u16(p, f->beacons);
    *p++                      = f->guess;
    *p++                      = f->flags;
    *p++                      = f->last_type;
    *p++                      = f->src_mode;
    p                         = put_u64(p, f->src_addr);
    memcpy(p, f->ext_pan_id, 8);
    p        += 8;
    *p++      = f->stack_profile;
    // NUL-padded; a 16-character name has no terminator on the wire
    size_t i  = 0;
    for (; i < I154_NAME_LEN && f->name[i]; i++) {
        p[i] = (uint8_t)f->name[i];
    }
    memset(p + i, 0, I154_NAME_LEN - i);
}

static void dec_frame(uint8_t const *p, i154_msg_t *m) {
    i154_frame_sum_t *f = &m->u.frame;
    f->seq              = *p++;
    f->channel          = *p++;
    f->pan_id           = get_u16(&p);
    f->rssi_last        = (int8_t)*p++;
    f->rssi_best        = (int8_t)*p++;
    f->lqi_last         = *p++;
    f->frames           = get_u16(&p);
    f->beacons          = get_u16(&p);
    f->guess            = *p++;
    f->flags            = *p++;
    f->last_type        = *p++;
    f->src_mode         = *p++;
    f->src_addr         = get_u64(&p);
    memcpy(f->ext_pan_id, p, 8);
    p                += 8;
    f->stack_profile  = *p++;
    memcpy(f->name, p, I154_NAME_LEN);
    f->name[I154_NAME_LEN] = '\0';
}

static void enc_done(i154_msg_t const *m, uint8_t *p) {
    i154_done_t const *d = &m->u.done;
    *p++                 = d->seq;
    *p++                 = d->code;
    p                    = put_u32(p, d->frames_rx);
    p                    = put_u16(p, d->frames_bad);
    *p++                 = d->networks;
    put_u16(p, d->dropped);
}

static void dec_done(uint8_t const *p, i154_msg_t *m) {
    i154_done_t *d = &m->u.done;
    d->seq         = *p++;
    d->code        = *p++;
    d->frames_rx   = get_u32(&p);
    d->frames_bad  = get_u16(&p);
    d->networks    = *p++;
    d->dropped     = get_u16(&p);
}

typedef struct {
    uint8_t type;
    uint8_t len;
    void (*enc)(i154_msg_t const *m, uint8_t *body);
    void (*dec)(uint8_t const *body, i154_msg_t *m);
} msg_layout_t;

static msg_layout_t const layouts[] = {
    {I154_MSG_CAPS_REQ, CAPS_REQ_LEN, enc_none, dec_none},
    {I154_MSG_SCAN_START, SCAN_START_LEN, enc_scan, dec_scan},
    {I154_MSG_SCAN_STOP, SCAN_STOP_LEN, enc_stop, dec_stop},
    {I154_MSG_CAPS, CAPS_LEN, enc_caps, dec_caps},
    {I154_MSG_ED, ED_LEN, enc_ed, dec_ed},
    {I154_MSG_FRAME, FRAME_LEN, enc_frame, dec_frame},
    {I154_MSG_DONE, DONE_LEN, enc_done, dec_done},
};

static msg_layout_t const *layout_of(uint8_t type) {
    for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
        if (layouts[i].type == type) {
            return &layouts[i];
        }
    }
    return NULL;
}

size_t i154_encode(i154_msg_t const *msg, uint8_t *buf, size_t cap) {
    msg_layout_t const *l = layout_of(msg->type);
    if (!l || l->len > cap) {
        return 0;
    }
    buf[0] = I154_PROTO_VERSION;
    buf[1] = msg->type;
    l->enc(msg, buf + HDR_LEN);
    return l->len;
}

i154_dec_t i154_decode(uint8_t const *buf, size_t len, i154_msg_t *out) {
    if (len < HDR_LEN) {
        return I154_DEC_BAD_LEN;
    }
    if (buf[0] != I154_PROTO_VERSION) {
        return I154_DEC_BAD_VERSION;
    }
    msg_layout_t const *l = layout_of(buf[1]);
    if (!l) {
        return I154_DEC_BAD_TYPE;
    }
    if (len != l->len) {
        return I154_DEC_BAD_LEN;
    }
    memset(out, 0, sizeof(*out));
    out->type = buf[1];
    l->dec(buf + HDR_LEN, out);
    return I154_DEC_OK;
}

// ---- scan requests ----

static bool dwell_range(uint8_t mode, uint16_t *lo, uint16_t *hi) {
    switch (mode) {
        case I154_MODE_ED: *lo = I154_ED_DWELL_MIN_MS, *hi = I154_ED_DWELL_MAX_MS; return true;
        case I154_MODE_PASSIVE: *lo = I154_LISTEN_DWELL_MIN_MS, *hi = I154_PASSIVE_DWELL_MAX_MS; return true;
        case I154_MODE_ACTIVE: *lo = I154_LISTEN_DWELL_MIN_MS, *hi = I154_ACTIVE_DWELL_MAX_MS; return true;
        default: return false;
    }
}

static uint32_t channel_count(uint32_t mask) {
    uint32_t n = 0;
    for (mask &= I154_CHANNEL_MASK_ALL; mask; mask &= mask - 1) {
        n++;
    }
    return n;
}

bool i154_scan_req_valid(i154_scan_req_t const *r) {
    uint16_t lo, hi;
    if (!dwell_range(r->mode, &lo, &hi) || r->dwell_ms < lo || r->dwell_ms > hi) {
        return false;
    }
    if (r->channel_mask == 0 || (r->channel_mask & ~I154_CHANNEL_MASK_ALL) != 0) {
        return false;
    }
    return channel_count(r->channel_mask) * r->dwell_ms <= I154_SCAN_MAX_TOTAL_MS;
}

bool i154_caps_allow(i154_caps_t const *caps, i154_scan_req_t const *r) {
    return i154_scan_req_valid(r) && r->mode < 8 && (caps->modes & I154_MODE_BIT(r->mode)) != 0 &&
           (r->channel_mask & ~caps->channel_mask) == 0;
}

uint32_t i154_scan_expected_ms(i154_scan_req_t const *r) {
    return channel_count(r->channel_mask) * ((uint32_t)r->dwell_ms + I154_CHANNEL_OVERHEAD_MS);
}

bool i154_scan_overdue(i154_scan_req_t const *r, uint32_t started_ms, uint32_t now_ms) {
    return now_ms - started_ms > i154_scan_expected_ms(r) + I154_SCAN_GRACE_MS;
}

// ---- MAC header parser ----

typedef struct {
    uint8_t const *buf;
    size_t         len;
    size_t         off;
} reader_t;

static uint8_t const *take(reader_t *r, size_t n) {
    if (n > r->len - r->off) {
        return NULL;
    }
    uint8_t const *p  = r->buf + r->off;
    r->off           += n;
    return p;
}

static bool take_le(reader_t *r, size_t n, uint64_t *v) {
    uint8_t const *p = take(r, n);
    if (!p) {
        return false;
    }
    *v = 0;
    for (size_t i = n; i-- > 0;) {
        *v = *v << 8 | p[i];
    }
    return true;
}

static size_t addr_len(uint8_t mode) {
    return mode == I154_ADDR_SHORT ? 2 : mode == I154_ADDR_EXT ? 8 : 0;
}

// Which PAN IDs the header carries: 802.15.4-2006 7.2.1.5, and table 7-2 of -2015 for frame version 2
static void pan_presence(i154_mac_t const *m, bool *dst, bool *src) {
    bool has_dst = m->dst_mode != I154_ADDR_NONE, has_src = m->src_mode != I154_ADDR_NONE;
    if (m->version < 2) {
        *dst = has_dst;
        *src = has_src && !(m->pan_comp && has_dst);
        return;
    }
    if (!has_dst && !has_src) {
        *dst = m->pan_comp, *src = false;
    } else if (has_dst != has_src) {
        *dst = has_dst && !m->pan_comp, *src = has_src && !m->pan_comp;
    } else if (m->dst_mode == I154_ADDR_EXT && m->src_mode == I154_ADDR_EXT) {
        *dst = !m->pan_comp, *src = false;
    } else {
        *dst = true, *src = !m->pan_comp;
    }
}

static i154_mac_res_t parse_fcf(reader_t *r, i154_mac_t *m) {
    uint64_t fcf;
    if (!take_le(r, 2, &fcf)) {
        return I154_MAC_SHORT;
    }
    m->frame_type = fcf & 0x7;
    m->security   = (fcf >> 3) & 1;
    m->pan_comp   = (fcf >> 6) & 1;
    m->dst_mode   = (fcf >> 10) & 3;
    m->version    = (fcf >> 12) & 3;
    m->src_mode   = (fcf >> 14) & 3;
    m->has_seq    = m->version < 2 || !((fcf >> 8) & 1); // sequence number suppression is a 2015 field
    m->ie_present = m->version >= 2 && ((fcf >> 9) & 1);
    if (m->version == 3 || m->dst_mode == 1 || m->src_mode == 1) {
        return I154_MAC_BAD;
    }
    return m->frame_type > I154_FT_CMD ? I154_MAC_UNSUPPORTED : I154_MAC_OK;
}

static bool parse_addrs(reader_t *r, i154_mac_t *m) {
    uint64_t v = 0;
    pan_presence(m, &m->has_dst_pan, &m->has_src_pan);
    if (m->has_dst_pan && !take_le(r, 2, &v)) {
        return false;
    }
    m->dst_pan = (uint16_t)v;
    if (!take_le(r, addr_len(m->dst_mode), &m->dst_addr)) {
        return false;
    }
    if (m->has_src_pan && !take_le(r, 2, &v)) {
        return false;
    }
    m->src_pan = (uint16_t)v;
    if (!take_le(r, addr_len(m->src_mode), &m->src_addr)) {
        return false;
    }
    if (!m->has_src_pan && m->src_mode != I154_ADDR_NONE && m->has_dst_pan) {
        m->has_src_pan = true; // compressed: the source shares the destination's PAN
        m->src_pan     = m->dst_pan;
    }
    return true;
}

// The auxiliary security header: control byte, frame counter (unless suppressed, 2015), key identifier
static bool skip_aux_security(reader_t *r, i154_mac_t const *m) {
    static uint8_t const key_id_len[4] = {0, 1, 5, 9};
    uint8_t const       *ctl           = take(r, 1);
    if (!ctl) {
        return false;
    }
    bool   counter_suppressed = m->version >= 2 && (*ctl >> 5) & 1;
    size_t n                  = (counter_suppressed ? 0 : 4) + key_id_len[(*ctl >> 3) & 3];
    return take(r, n) != NULL;
}

static void copy_name(char *dst, uint8_t const *src) {
    size_t i = 0;
    for (; i < I154_NAME_LEN && src[i]; i++) {
        dst[i] = src[i] < 0x20 || src[i] == 0x7F ? '?' : (char)src[i]; // the UI prints it
    }
    dst[i] = '\0';
}

// The beacon payload after superframe, GTS and pending address fields: Zigbee (protocol id 0) or Thread (3)
static void beacon_hints(uint8_t const *p, size_t n, i154_mac_t *m) {
    if (n >= 15 && p[0] == 0x00) {
        m->guess         = I154_NET_ZIGBEE;
        m->stack_profile = p[1] & 0x0F;
        m->has_ext_pan   = true;
        memcpy(m->ext_pan_id, p + 3, 8);
    } else if (n >= 26 && p[0] == 0x03) {
        m->guess       = I154_NET_THREAD;
        m->permit_join = p[1] & 1;
        copy_name(m->name, p + 2);
        m->has_ext_pan = true;
        memcpy(m->ext_pan_id, p + 18, 8);
    }
}

static bool parse_beacon(reader_t *r, i154_mac_t *m) {
    uint64_t       sf;
    uint8_t const *gts, *pend;
    if (!take_le(r, 2, &sf) || !(gts = take(r, 1))) {
        return false;
    }
    if ((*gts & 7) && !take(r, 1 + 3u * (*gts & 7))) {
        return false;
    }
    if (!(pend = take(r, 1)) || !take(r, 2u * (*pend & 7) + 8u * ((*pend >> 4) & 7))) {
        return false;
    }
    m->permit_join = (sf >> 15) & 1; // association permit
    beacon_hints(r->buf + r->off, r->len - r->off, m);
    return true;
}

static bool is_lowpan_dispatch(uint8_t b) {
    return b == 0x41 || (b & 0xE0) == 0x60 || (b & 0xC0) == 0x80 || (b & 0xF8) == 0xC0 || (b & 0xF8) == 0xE0;
}

// Zigbee NWK frame control: frame type data/command/inter-PAN, protocol version 2 or 3, discover route 0 or 1. These
// first bytes never are a 6LoWPAN dispatch, which is how Thread data starts
static i154_net_t data_guess(uint8_t const *p, size_t n) {
    if (n >= 8 && p[0] < 0x80 && (p[0] & 3) != 2 && ((p[0] & 0x3C) == 0x08 || (p[0] & 0x3C) == 0x0C)) {
        return I154_NET_ZIGBEE;
    }
    return n >= 1 && is_lowpan_dispatch(p[0]) ? I154_NET_THREAD : I154_NET_UNKNOWN;
}

// Zigbee leaves MAC security off (it secures at NWK level); Thread secures its MAC frames with frame version 2006+
static bool payload_hints(reader_t *r, i154_mac_t *m) {
    if (m->security || m->ie_present) {
        m->guess = m->version >= 1 ? I154_NET_THREAD : I154_NET_UNKNOWN;
        return true;
    }
    uint8_t const *p = r->buf + r->off;
    size_t         n = r->len - r->off;
    switch (m->frame_type) {
        case I154_FT_BEACON: return parse_beacon(r, m);
        case I154_FT_CMD:
            if (n == 0) {
                return false;
            }
            m->cmd_id = p[0];
            return true;
        case I154_FT_DATA: m->guess = data_guess(p, n); return true;
        default: return true;
    }
}

i154_mac_res_t i154_mac_parse(uint8_t const *frame, size_t len, i154_mac_t *out) {
    memset(out, 0, sizeof(*out));
    if (len > I154_PSDU_MAX) {
        return I154_MAC_BAD;
    }
    reader_t       r   = {frame, len, 0};
    i154_mac_res_t res = parse_fcf(&r, out);
    if (res != I154_MAC_OK) {
        return res;
    }
    uint8_t const *seq = out->has_seq ? take(&r, 1) : NULL;
    if (out->has_seq && !seq) {
        return I154_MAC_SHORT;
    }
    out->seq = seq ? *seq : 0;
    if (!parse_addrs(&r, out) || (out->security && !skip_aux_security(&r, out))) {
        return I154_MAC_SHORT;
    }
    out->hdr_len     = (uint8_t)r.off;
    out->payload_len = (uint8_t)(len - r.off);
    return payload_hints(&r, out) ? I154_MAC_OK : I154_MAC_SHORT;
}

bool i154_mac_pan(i154_mac_t const *m, uint16_t *pan) {
    if (m->has_src_pan && m->src_pan != 0xFFFF) {
        *pan = m->src_pan;
        return true;
    }
    if (m->has_dst_pan && m->dst_pan != 0xFFFF) {
        *pan = m->dst_pan;
        return true;
    }
    return false;
}

// ---- dedupe table ----

void i154_dedupe_init(i154_dedupe_t *t) {
    memset(t, 0, sizeof(*t));
}

static uint16_t sat_inc(uint16_t v) {
    return v == UINT16_MAX ? v : (uint16_t)(v + 1);
}

static int find_or_claim(i154_dedupe_t *t, uint8_t channel, uint16_t pan, uint32_t now_ms, bool *is_new) {
    int free_i = -1, oldest = 0;
    for (int i = 0; i < I154_DEDUPE_SIZE; i++) {
        i154_dedupe_entry_t const *e = &t->e[i];
        if (e->used && e->sum.channel == channel && e->sum.pan_id == pan) {
            *is_new = false;
            return i;
        }
        if (!e->used && free_i < 0) {
            free_i = i;
        }
        if (now_ms - e->last_ms > now_ms - t->e[oldest].last_ms) {
            oldest = i;
        }
    }
    if (free_i < 0) {
        free_i     = oldest;
        t->evicted = sat_inc(t->evicted);
    }
    i154_dedupe_entry_t *e = &t->e[free_i];
    memset(e, 0, sizeof(*e));
    e->used          = true;
    e->sum.channel   = channel;
    e->sum.pan_id    = pan;
    e->sum.rssi_best = INT8_MIN;
    *is_new          = true;
    return free_i;
}

static void vote(i154_dedupe_entry_t *e, uint8_t guess) {
    if (guess == I154_NET_ZIGBEE) {
        e->votes_zigbee = sat_inc(e->votes_zigbee);
    } else if (guess == I154_NET_THREAD) {
        e->votes_thread = sat_inc(e->votes_thread);
    }
    // A tie keeps the previous guess, so a network does not flip with every frame
    if (e->votes_zigbee > e->votes_thread) {
        e->sum.guess = I154_NET_ZIGBEE;
    } else if (e->votes_thread > e->votes_zigbee) {
        e->sum.guess = I154_NET_THREAD;
    }
}

static void take_beacon(i154_frame_sum_t *s, i154_mac_t const *m) {
    s->beacons  = sat_inc(s->beacons);
    s->flags   |= I154_FRAME_F_BEACON;
    s->flags    = m->permit_join ? s->flags | I154_FRAME_F_PERMIT_JOIN : s->flags & ~I154_FRAME_F_PERMIT_JOIN;
    if (m->has_ext_pan) {
        s->flags |= I154_FRAME_F_EXT_PAN;
        memcpy(s->ext_pan_id, m->ext_pan_id, 8);
        s->stack_profile = m->stack_profile;
    }
    if (m->name[0]) {
        s->flags |= I154_FRAME_F_NAME;
        memcpy(s->name, m->name, sizeof(s->name));
    }
}

static void take_frame(i154_dedupe_entry_t *e, int8_t rssi, uint8_t lqi, i154_mac_t const *m) {
    i154_frame_sum_t *s = &e->sum;
    s->frames           = sat_inc(s->frames);
    s->rssi_last        = rssi;
    s->rssi_best        = rssi > s->rssi_best ? rssi : s->rssi_best;
    s->lqi_last         = lqi;
    s->last_type        = m->frame_type;
    if (m->src_mode != I154_ADDR_NONE) {
        s->src_mode = m->src_mode;
        s->src_addr = m->src_addr;
    }
    if (m->security) {
        s->flags |= I154_FRAME_F_SECURED;
    }
    if (m->frame_type == I154_FT_BEACON) {
        take_beacon(s, m);
    }
    vote(e, m->guess);
}

static bool summary_changed(i154_frame_sum_t const *a, i154_frame_sum_t const *b) {
    return a->guess != b->guess || a->flags != b->flags || a->src_mode != b->src_mode ||
           a->stack_profile != b->stack_profile || memcmp(a->ext_pan_id, b->ext_pan_id, 8) != 0 ||
           strcmp(a->name, b->name) != 0;
}

i154_dd_res_t i154_dedupe_add(
    i154_dedupe_t *t, uint8_t channel, int8_t rssi, uint8_t lqi, i154_mac_t const *m, uint32_t now_ms, int *index
) {
    uint16_t pan;
    if (!i154_mac_pan(m, &pan)) {
        return I154_DD_IGNORED;
    }
    bool                 is_new;
    int                  i      = find_or_claim(t, channel, pan, now_ms, &is_new);
    i154_dedupe_entry_t *e      = &t->e[i];
    i154_frame_sum_t     before = e->sum;
    e->last_ms                  = now_ms;
    take_frame(e, rssi, lqi, m);
    if (index) {
        *index = i;
    }
    if (is_new) {
        return I154_DD_NEW;
    }
    return summary_changed(&before, &e->sum) ? I154_DD_CHANGED : I154_DD_SEEN;
}

int i154_dedupe_count(i154_dedupe_t const *t) {
    int n = 0;
    for (int i = 0; i < I154_DEDUPE_SIZE; i++) {
        n += t->e[i].used;
    }
    return n;
}

// ---- capability probe ----

void i154_probe_start(i154_probe_t *p) {
    memset(p, 0, sizeof(*p));
    p->state = I154_PROBE_PENDING;
}

bool i154_probe_poll(i154_probe_t *p, uint32_t now_ms) {
    if (p->state != I154_PROBE_PENDING) {
        return false;
    }
    if (p->sent > 0 && now_ms - p->last_ms < I154_PROBE_INTERVAL_MS) {
        return false;
    }
    if (p->sent >= I154_PROBE_TRIES) {
        p->state = I154_PROBE_C6_TOO_OLD;
        return false;
    }
    p->sent++;
    p->last_ms = now_ms;
    return true;
}

void i154_probe_on_msg(i154_probe_t *p, uint8_t const *buf, size_t len) {
    if (p->state != I154_PROBE_PENDING || len < HDR_LEN) {
        return;
    }
    if (buf[0] != I154_PROTO_VERSION) {
        p->state = buf[0] < I154_PROTO_VERSION ? I154_PROBE_C6_TOO_OLD : I154_PROBE_C6_NEWER;
        return;
    }
    i154_msg_t msg;
    if (i154_decode(buf, len, &msg) == I154_DEC_OK && msg.type == I154_MSG_CAPS) {
        p->caps  = msg.u.caps;
        p->state = I154_PROBE_OK;
    }
}
