// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
// The LoRa message decoder exactly as it was before the #212 split (shared/lora/lora_proto.c at c5ef5200), renamed
// lora_decode_ref. Test only: lora_decode_pin_test compares the live decoder against it, byte for byte.
#include "lora_decode_ref.h"

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

static uint16_t get_u16(uint8_t const **p) {
    uint16_t v  = (uint16_t)((*p)[0] | (*p)[1] << 8);
    *p         += 2;
    return v;
}

static uint32_t get_u32(uint8_t const **p) {
    uint32_t lo = get_u16(p);
    return lo | (uint32_t)get_u16(p) << 16;
}

// The registry messages (#308) sit in helpers so lora_encode / lora_decode keep their complexity
static size_t reg_encoded_len(uint8_t type) {
    switch (type) {
        case LORA_MSG_REG_GET: return REG_GET_LEN;
        case LORA_MSG_REG_SET: return REG_SET_LEN;
        case LORA_MSG_REG_VAL: return REG_VAL_LEN;
        default: return 0;
    }
}

static lora_dec_t decode_reg(uint8_t const *buf, size_t len, lora_msg_t *out) {
    size_t want = reg_encoded_len(out->type);
    if (!want) {
        return LORA_DEC_BAD_TYPE;
    }
    if (len != want) {
        return LORA_DEC_BAD_LEN;
    }
    uint8_t const *p = buf + HDR_LEN;
    if (out->type == LORA_MSG_REG_GET) {
        out->u.reg_get.id  = get_u16(&p);
        out->u.reg_get.seq = *p;
    } else if (out->type == LORA_MSG_REG_SET) {
        out->u.reg_set.id    = get_u16(&p);
        out->u.reg_set.value = get_u32(&p);
        out->u.reg_set.seq   = *p;
    } else {
        lora_reg_val_t *v = &out->u.reg_val;
        v->id             = get_u16(&p);
        v->seq            = *p++;
        v->status         = *p++;
        v->type           = *p++;
        v->value          = get_u32(&p);
    }
    return LORA_DEC_OK;
}

static lora_dec_t decode_sweep(uint8_t const *buf, size_t len, lora_sweep_t *sw) {
    if (len < SWEEP_FIXED_LEN) {
        return LORA_DEC_BAD_LEN;
    }
    uint8_t count = buf[SWEEP_FIXED_LEN - 1];
    if (count == 0 || count > LORA_SWEEP_MAX_BINS || len != SWEEP_FIXED_LEN + 2u * count) {
        return LORA_DEC_BAD_LEN;
    }
    uint8_t const *p     = buf + HDR_LEN;
    sw->start_hz         = get_u32(&p);
    sw->step_hz          = get_u32(&p);
    sw->seq              = *p++;
    sw->channel_rssi_x10 = (int16_t)get_u16(&p);
    sw->count            = *p++;
    for (uint8_t i = 0; i < count; i++) {
        sw->rssi_x10[i] = (int16_t)get_u16(&p);
    }
    return LORA_DEC_OK;
}

lora_dec_t lora_decode_ref(uint8_t const *buf, size_t len, lora_msg_t *out) {
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
        case LORA_MSG_STATUS_REQ:
        case LORA_MSG_IDENT_REQ: return len == BODYLESS_LEN ? LORA_DEC_OK : LORA_DEC_BAD_LEN;
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
        case LORA_MSG_IDENT:
            if (len != IDENT_LEN) {
                return LORA_DEC_BAD_LEN;
            }
            memcpy(out->u.ident.app_elf_sha256, p, LORA_IDENT_SHA_LEN);
            return LORA_DEC_OK;
        case LORA_MSG_SWEEP_REQ: {
            if (len != SWEEP_REQ_LEN) {
                return LORA_DEC_BAD_LEN;
            }
            lora_sweep_req_t *r = &out->u.sweep_req;
            r->start_hz         = get_u32(&p);
            r->step_hz          = get_u32(&p);
            r->count            = *p++;
            r->samples          = *p++;
            r->seq              = *p;
            return LORA_DEC_OK;
        }
        case LORA_MSG_SWEEP: return decode_sweep(buf, len, &out->u.sweep);
        default: return decode_reg(buf, len, out); // BAD_TYPE for anything else
    }
}
