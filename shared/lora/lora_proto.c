// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
#include "lora_proto.h"

#include <string.h>

#define HDR_LEN         2
#define CONFIG_LEN      (HDR_LEN + 14)
#define RX_FIXED_LEN    (HDR_LEN + 9) // up to and including the length byte
#define TX_FIXED_LEN    (HDR_LEN + 1) // the length byte
#define STATUS_LEN      (HDR_LEN + 39)
#define RESULT_LEN      (HDR_LEN + 4)
#define IDENT_LEN       (HDR_LEN + LORA_IDENT_SHA_LEN)
#define SWEEP_REQ_LEN   (HDR_LEN + 11)
#define SWEEP_FIXED_LEN (HDR_LEN + 12) // up to and including the count byte; 2 bytes per step follow
#define BODYLESS_LEN    HDR_LEN
#define REG_GET_LEN     (HDR_LEN + 3)
#define REG_SET_LEN     (HDR_LEN + 7)
#define REG_VAL_LEN     (HDR_LEN + 9)

static uint8_t *put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return p + 2;
}

static uint8_t *put_u32(uint8_t *p, uint32_t v) {
    p = put_u16(p, (uint16_t)v);
    return put_u16(p, (uint16_t)(v >> 16));
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

// The registry messages (#308) sit in helpers so lora_encode keeps its complexity
static size_t reg_encoded_len(uint8_t type) {
    switch (type) {
        case LORA_MSG_REG_GET: return REG_GET_LEN;
        case LORA_MSG_REG_SET: return REG_SET_LEN;
        case LORA_MSG_REG_VAL: return REG_VAL_LEN;
        default: return 0;
    }
}

static void encode_reg(lora_msg_t const *msg, uint8_t *p) {
    switch (msg->type) {
        case LORA_MSG_REG_GET:
            p  = put_u16(p, msg->u.reg_get.id);
            *p = msg->u.reg_get.seq;
            break;
        case LORA_MSG_REG_SET:
            p  = put_u16(p, msg->u.reg_set.id);
            p  = put_u32(p, msg->u.reg_set.value);
            *p = msg->u.reg_set.seq;
            break;
        case LORA_MSG_REG_VAL: {
            lora_reg_val_t const *v = &msg->u.reg_val;
            p                       = put_u16(p, v->id);
            *p++                    = v->seq;
            *p++                    = v->status;
            *p++                    = v->type;
            put_u32(p, v->value);
            break;
        }
        default: break; // bodyless
    }
}

static size_t encoded_len(lora_msg_t const *msg) {
    switch (msg->type) {
        case LORA_MSG_CONFIG: return CONFIG_LEN;
        case LORA_MSG_RX_START:
        case LORA_MSG_RX_STOP:
        case LORA_MSG_STATUS_REQ:
        case LORA_MSG_IDENT_REQ: return BODYLESS_LEN;
        case LORA_MSG_SEND: return msg->u.tx.len ? TX_FIXED_LEN + msg->u.tx.len : 0;
        case LORA_MSG_RX_PACKET: return RX_FIXED_LEN + msg->u.rx.len;
        case LORA_MSG_STATUS: return STATUS_LEN;
        case LORA_MSG_RESULT: return RESULT_LEN;
        case LORA_MSG_IDENT: return IDENT_LEN;
        case LORA_MSG_SWEEP_REQ: return SWEEP_REQ_LEN;
        case LORA_MSG_SWEEP: {
            uint8_t count = msg->u.sweep.count;
            return count && count <= LORA_SWEEP_MAX_BINS ? SWEEP_FIXED_LEN + 2u * count : 0;
        }
        default: return reg_encoded_len(msg->type);
    }
}

size_t lora_encode(lora_msg_t const *msg, uint8_t *buf, size_t cap) {
    size_t len = encoded_len(msg);
    if (!len || len > cap) {
        return 0;
    }

    uint8_t *p = buf;
    *p++       = LORA_PROTO_VERSION;
    *p++       = msg->type;
    switch (msg->type) {
        case LORA_MSG_CONFIG: {
            lora_config_t const *c = &msg->u.config;
            p                      = put_u32(p, c->freq_hz);
            p                      = put_u32(p, c->bw_hz);
            *p++                   = c->sf;
            *p++                   = c->cr;
            *p++                   = c->sync_word;
            p                      = put_u16(p, c->preamble);
            *p                     = (uint8_t)c->tx_power_dbm;
            break;
        }
        case LORA_MSG_SEND: {
            *p++ = msg->u.tx.len;
            memcpy(p, msg->u.tx.data, msg->u.tx.len);
            break;
        }
        case LORA_MSG_RX_PACKET: {
            lora_rx_packet_t const *rx = &msg->u.rx;
            p                          = put_u16(p, (uint16_t)rx->rssi_x10);
            p                          = put_u16(p, (uint16_t)rx->snr_x10);
            p                          = put_u32(p, rx->c6_ms);
            *p++                       = rx->len;
            memcpy(p, rx->data, rx->len);
            break;
        }
        case LORA_MSG_STATUS: {
            lora_status_t const *s = &msg->u.status;
            *p++                   = s->state;
            p                      = put_u16(p, (uint16_t)s->rssi_x10);
            p                      = put_u16(p, (uint16_t)s->last_radio_err);
            p                      = put_u32(p, s->packets);
            p                      = put_u32(p, s->crc_errors);
            p                      = put_u32(p, s->header_errors);
            p                      = put_u32(p, s->dropped);
            p                      = put_u32(p, s->ignored);
            p                      = put_u32(p, s->uptime_s);
            p                      = put_u32(p, s->heap_free);
            p                      = put_u32(p, s->heap_min);
            put_u16(p, s->stack_free);
            break;
        }
        case LORA_MSG_RESULT: {
            lora_result_t const *r = &msg->u.result;
            *p++                   = r->for_type;
            *p++                   = r->code;
            put_u16(p, (uint16_t)r->radio_err);
            break;
        }
        case LORA_MSG_IDENT: memcpy(p, msg->u.ident.app_elf_sha256, LORA_IDENT_SHA_LEN); break;
        case LORA_MSG_SWEEP_REQ: {
            lora_sweep_req_t const *r = &msg->u.sweep_req;
            p                         = put_u32(p, r->start_hz);
            p                         = put_u32(p, r->step_hz);
            *p++                      = r->count;
            *p++                      = r->samples;
            *p                        = r->seq;
            break;
        }
        case LORA_MSG_SWEEP: {
            lora_sweep_t const *sw = &msg->u.sweep;
            p                      = put_u32(p, sw->start_hz);
            p                      = put_u32(p, sw->step_hz);
            *p++                   = sw->seq;
            p                      = put_u16(p, (uint16_t)sw->channel_rssi_x10);
            *p++                   = sw->count;
            for (uint8_t i = 0; i < sw->count; i++) {
                p = put_u16(p, (uint16_t)sw->rssi_x10[i]);
            }
            break;
        }
        default: encode_reg(msg, p); break; // the registry messages, or bodyless
    }
    return len;
}

// The decoder, one entry per message type (#212). A frame's length is checked first, then its body read, so a
// wrong length never reads a byte of the body. Fixed-length types give exact_len; the ones whose length follows from a
// length byte give len_ok. The readers return nothing and end in plain stores or a memcpy, so the compiler adds no
// stack frame under lora_decode's (the C6 runs this on its LoRa task).
typedef struct {
    uint8_t type;      // lora_msg_type_t
    uint8_t exact_len; // 0: len_ok decides
    bool (*len_ok)(uint8_t const *buf, size_t len);
    void (*read)(uint8_t const *p, lora_msg_t *out); // p: the body after the header; NULL for the bodyless commands
} decoder_t;

// [header][fixed fields][n][n bytes], n 1-255, the length byte the last fixed byte
static bool counted_len_ok(uint8_t const *buf, size_t len, size_t fixed) {
    return len >= fixed && buf[fixed - 1] != 0 && len == fixed + buf[fixed - 1];
}

static bool send_len_ok(uint8_t const *buf, size_t len) {
    return counted_len_ok(buf, len, TX_FIXED_LEN);
}

static bool rx_len_ok(uint8_t const *buf, size_t len) {
    return counted_len_ok(buf, len, RX_FIXED_LEN);
}

static bool sweep_len_ok(uint8_t const *buf, size_t len) {
    if (len < SWEEP_FIXED_LEN) {
        return false;
    }
    uint8_t count = buf[SWEEP_FIXED_LEN - 1];
    return count != 0 && count <= LORA_SWEEP_MAX_BINS && len == SWEEP_FIXED_LEN + 2u * count;
}

static void read_config(uint8_t const *p, lora_msg_t *out) {
    lora_config_t *c = &out->u.config;
    c->freq_hz       = get_u32(&p);
    c->bw_hz         = get_u32(&p);
    c->sf            = *p++;
    c->cr            = *p++;
    c->sync_word     = *p++;
    c->preamble      = get_u16(&p);
    c->tx_power_dbm  = (int8_t)*p;
}

static void read_send(uint8_t const *p, lora_msg_t *out) {
    out->u.tx.len = *p++;
    memcpy(out->u.tx.data, p, out->u.tx.len);
}

static void read_rx(uint8_t const *p, lora_msg_t *out) {
    lora_rx_packet_t *rx = &out->u.rx;
    rx->rssi_x10         = (int16_t)get_u16(&p);
    rx->snr_x10          = (int16_t)get_u16(&p);
    rx->c6_ms            = get_u32(&p);
    rx->len              = *p++;
    memcpy(rx->data, p, rx->len);
}

static void read_status(uint8_t const *p, lora_msg_t *out) {
    lora_status_t *s  = &out->u.status;
    s->state          = *p++;
    s->rssi_x10       = (int16_t)get_u16(&p);
    s->last_radio_err = (int16_t)get_u16(&p);
    s->packets        = get_u32(&p);
    s->crc_errors     = get_u32(&p);
    s->header_errors  = get_u32(&p);
    s->dropped        = get_u32(&p);
    s->ignored        = get_u32(&p);
    s->uptime_s       = get_u32(&p);
    s->heap_free      = get_u32(&p);
    s->heap_min       = get_u32(&p);
    s->stack_free     = get_u16(&p);
}

static void read_result(uint8_t const *p, lora_msg_t *out) {
    lora_result_t *r = &out->u.result;
    r->for_type      = *p++;
    r->code          = *p++;
    r->radio_err     = (int16_t)get_u16(&p);
}

static void read_ident(uint8_t const *p, lora_msg_t *out) {
    memcpy(out->u.ident.app_elf_sha256, p, LORA_IDENT_SHA_LEN);
}

static void read_sweep_req(uint8_t const *p, lora_msg_t *out) {
    lora_sweep_req_t *r = &out->u.sweep_req;
    r->start_hz         = get_u32(&p);
    r->step_hz          = get_u32(&p);
    r->count            = *p++;
    r->samples          = *p++;
    r->seq              = *p;
}

static void read_sweep(uint8_t const *p, lora_msg_t *out) {
    lora_sweep_t *sw     = &out->u.sweep;
    sw->start_hz         = get_u32(&p);
    sw->step_hz          = get_u32(&p);
    sw->seq              = *p++;
    sw->channel_rssi_x10 = (int16_t)get_u16(&p);
    sw->count            = *p++;
    for (uint8_t i = 0; i < sw->count; i++) {
        sw->rssi_x10[i] = (int16_t)get_u16(&p);
    }
}

static void read_reg_get(uint8_t const *p, lora_msg_t *out) {
    out->u.reg_get.id  = get_u16(&p);
    out->u.reg_get.seq = *p;
}

static void read_reg_set(uint8_t const *p, lora_msg_t *out) {
    out->u.reg_set.id    = get_u16(&p);
    out->u.reg_set.value = get_u32(&p);
    out->u.reg_set.seq   = *p;
}

static void read_reg_val(uint8_t const *p, lora_msg_t *out) {
    lora_reg_val_t *v = &out->u.reg_val;
    v->id             = get_u16(&p);
    v->seq            = *p++;
    v->status         = *p++;
    v->type           = *p++;
    v->value          = get_u32(&p);
}

static decoder_t const decoders[] = {
    {LORA_MSG_CONFIG, CONFIG_LEN, NULL, read_config},
    {LORA_MSG_RX_START, BODYLESS_LEN, NULL, NULL},
    {LORA_MSG_RX_STOP, BODYLESS_LEN, NULL, NULL},
    {LORA_MSG_STATUS_REQ, BODYLESS_LEN, NULL, NULL},
    {LORA_MSG_SEND, 0, send_len_ok, read_send},
    {LORA_MSG_IDENT_REQ, BODYLESS_LEN, NULL, NULL},
    {LORA_MSG_SWEEP_REQ, SWEEP_REQ_LEN, NULL, read_sweep_req},
    {LORA_MSG_REG_GET, REG_GET_LEN, NULL, read_reg_get},
    {LORA_MSG_REG_SET, REG_SET_LEN, NULL, read_reg_set},
    {LORA_MSG_RX_PACKET, 0, rx_len_ok, read_rx},
    {LORA_MSG_STATUS, STATUS_LEN, NULL, read_status},
    {LORA_MSG_RESULT, RESULT_LEN, NULL, read_result},
    {LORA_MSG_IDENT, IDENT_LEN, NULL, read_ident},
    {LORA_MSG_SWEEP, 0, sweep_len_ok, read_sweep},
    {LORA_MSG_REG_VAL, REG_VAL_LEN, NULL, read_reg_val},
};

static decoder_t const *find_decoder(uint8_t type) {
    for (size_t i = 0; i < sizeof(decoders) / sizeof(decoders[0]); i++) {
        if (decoders[i].type == type) {
            return &decoders[i];
        }
    }
    return NULL;
}

// Which error wins: a frame shorter than the header is BAD_LEN, then the version, then the type, then the length.
// out is cleared (and its type set) only once the version matched, as before #212.
lora_dec_t lora_decode(uint8_t const *buf, size_t len, lora_msg_t *out) {
    if (len < HDR_LEN) {
        return LORA_DEC_BAD_LEN;
    }
    if (buf[0] != LORA_PROTO_VERSION) {
        return LORA_DEC_BAD_VERSION;
    }

    memset(out, 0, sizeof(*out));
    out->type            = buf[1];
    decoder_t const *dec = find_decoder(out->type);
    if (!dec) {
        return LORA_DEC_BAD_TYPE;
    }
    if (dec->exact_len ? len != dec->exact_len : !dec->len_ok(buf, len)) {
        return LORA_DEC_BAD_LEN;
    }
    if (dec->read) {
        dec->read(buf + HDR_LEN, out);
    }
    return LORA_DEC_OK;
}

bool lora_config_valid(lora_config_t const *c) {
    static uint32_t const bandwidths[] = {7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000};

    bool bw_ok = false;
    for (size_t i = 0; i < sizeof(bandwidths) / sizeof(bandwidths[0]); i++) {
        bw_ok |= c->bw_hz == bandwidths[i];
    }
    return bw_ok && c->freq_hz >= 862000000 && c->freq_hz <= 928000000 && c->sf >= 5 && c->sf <= 12 && c->cr >= 5 &&
           c->cr <= 8 && c->preamble >= 6 && c->tx_power_dbm >= -9 && c->tx_power_dbm <= 22;
}

bool lora_sweep_req_valid(lora_sweep_req_t const *r) {
    if (r->count == 0 || r->count > LORA_SWEEP_MAX_BINS || r->samples == 0 || r->samples > LORA_SWEEP_MAX_SAMPLES ||
        r->step_hz < LORA_SWEEP_MIN_STEP_HZ || r->step_hz > LORA_SWEEP_MAX_STEP_HZ) {
        return false;
    }
    uint64_t last = (uint64_t)r->start_hz + (uint64_t)r->step_hz * (r->count - 1u);
    return r->start_hz >= 862000000 && last <= 928000000;
}
