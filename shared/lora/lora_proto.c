// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
#include "lora_proto.h"

#include <string.h>

#define HDR_LEN      2
#define CONFIG_LEN   (HDR_LEN + 14)
#define RX_FIXED_LEN (HDR_LEN + 9) // up to and including the length byte
#define TX_FIXED_LEN (HDR_LEN + 1) // the length byte
#define STATUS_LEN   (HDR_LEN + 39)
#define RESULT_LEN   (HDR_LEN + 4)
#define BODYLESS_LEN HDR_LEN

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

static size_t encoded_len(lora_msg_t const *msg) {
    switch (msg->type) {
        case LORA_MSG_CONFIG: return CONFIG_LEN;
        case LORA_MSG_RX_START:
        case LORA_MSG_RX_STOP:
        case LORA_MSG_STATUS_REQ: return BODYLESS_LEN;
        case LORA_MSG_SEND: return msg->u.tx.len ? TX_FIXED_LEN + msg->u.tx.len : 0;
        case LORA_MSG_RX_PACKET: return RX_FIXED_LEN + msg->u.rx.len;
        case LORA_MSG_STATUS: return STATUS_LEN;
        case LORA_MSG_RESULT: return RESULT_LEN;
        default: return 0;
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
        default: break; // bodyless
    }
    return len;
}

lora_dec_t lora_decode(uint8_t const *buf, size_t len, lora_msg_t *out) {
    if (len < HDR_LEN) {
        return LORA_DEC_BAD_LEN;
    }
    if (buf[0] != LORA_PROTO_VERSION) {
        return LORA_DEC_BAD_VERSION;
    }

    uint8_t const *p = buf + HDR_LEN;
    memset(out, 0, sizeof(*out));
    out->type = buf[1];
    switch (out->type) {
        case LORA_MSG_CONFIG: {
            if (len != CONFIG_LEN) {
                return LORA_DEC_BAD_LEN;
            }
            lora_config_t *c = &out->u.config;
            c->freq_hz       = get_u32(&p);
            c->bw_hz         = get_u32(&p);
            c->sf            = *p++;
            c->cr            = *p++;
            c->sync_word     = *p++;
            c->preamble      = get_u16(&p);
            c->tx_power_dbm  = (int8_t)*p;
            return LORA_DEC_OK;
        }
        case LORA_MSG_RX_START:
        case LORA_MSG_RX_STOP:
        case LORA_MSG_STATUS_REQ: return len == BODYLESS_LEN ? LORA_DEC_OK : LORA_DEC_BAD_LEN;
        case LORA_MSG_SEND: {
            if (len < TX_FIXED_LEN || buf[TX_FIXED_LEN - 1] == 0 ||
                len != (size_t)TX_FIXED_LEN + buf[TX_FIXED_LEN - 1]) {
                return LORA_DEC_BAD_LEN;
            }
            out->u.tx.len = *p++;
            memcpy(out->u.tx.data, p, out->u.tx.len);
            return LORA_DEC_OK;
        }
        case LORA_MSG_RX_PACKET: {
            if (len < RX_FIXED_LEN || buf[RX_FIXED_LEN - 1] == 0 ||
                len != (size_t)RX_FIXED_LEN + buf[RX_FIXED_LEN - 1]) {
                return LORA_DEC_BAD_LEN;
            }
            lora_rx_packet_t *rx = &out->u.rx;
            rx->rssi_x10         = (int16_t)get_u16(&p);
            rx->snr_x10          = (int16_t)get_u16(&p);
            rx->c6_ms            = get_u32(&p);
            rx->len              = *p++;
            memcpy(rx->data, p, rx->len);
            return LORA_DEC_OK;
        }
        case LORA_MSG_STATUS: {
            if (len != STATUS_LEN) {
                return LORA_DEC_BAD_LEN;
            }
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
            return LORA_DEC_OK;
        }
        case LORA_MSG_RESULT: {
            if (len != RESULT_LEN) {
                return LORA_DEC_BAD_LEN;
            }
            lora_result_t *r = &out->u.result;
            r->for_type      = *p++;
            r->code          = *p++;
            r->radio_err     = (int16_t)get_u16(&p);
            return LORA_DEC_OK;
        }
        default: return LORA_DEC_BAD_TYPE;
    }
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
