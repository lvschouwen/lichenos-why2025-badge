#include "mesh_frame.h"

#include <string.h>

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(uint8_t const *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

size_t mf_encode_device_query(uint8_t *out, uint8_t app_ver) {
    out[0] = MF_CMD_DEVICE_QUERY;
    out[1] = app_ver;
    return 2;
}

size_t mf_encode_app_start(uint8_t *out, char const *app_name) {
    size_t n = strlen(app_name);
    if (8 + n > MF_MAX_FRAME) {
        return 0;
    }
    memset(out, 0, 8); // the app version byte and six reserved: MyMesh reads neither
    out[0] = MF_CMD_APP_START;
    memcpy(out + 8, app_name, n);
    return 8 + n;
}

size_t mf_encode_sync_next(uint8_t *out) {
    out[0] = MF_CMD_SYNC_NEXT;
    return 1;
}

size_t mf_encode_send_channel(uint8_t *out, uint8_t channel_idx, uint32_t timestamp, char const *text,
                              size_t text_len) {
    if (text_len > MF_MAX_TEXT || 7 + text_len > MF_MAX_FRAME) {
        return 0;
    }
    out[0] = MF_CMD_SEND_CHANNEL_TXT;
    out[1] = 0; // TXT_TYPE_PLAIN
    out[2] = channel_idx;
    put32(out + 3, timestamp);
    memcpy(out + 7, text, text_len);
    return 7 + text_len;
}

size_t mf_encode_send_dm(uint8_t *out, uint8_t attempt, uint32_t timestamp, uint8_t const prefix[MF_PREFIX],
                         char const *text, size_t text_len) {
    if (text_len > MF_MAX_TEXT || 13 + text_len > MF_MAX_FRAME) {
        return 0;
    }
    out[0] = MF_CMD_SEND_TXT_MSG;
    out[1] = 0; // TXT_TYPE_PLAIN
    out[2] = attempt;
    put32(out + 3, timestamp);
    memcpy(out + 7, prefix, MF_PREFIX);
    memcpy(out + 13, text, text_len);
    return 13 + text_len;
}

size_t mf_encode_send_cli(uint8_t *out, uint8_t attempt, uint32_t timestamp, uint8_t const prefix[MF_PREFIX],
                          char const *text, size_t text_len) {
    size_t n = mf_encode_send_dm(out, attempt, timestamp, prefix, text, text_len);
    if (n) {
        out[1] = MF_TXT_CLI_DATA;
    }
    return n;
}

size_t mf_encode_login(uint8_t *out, uint8_t const pub[MF_PUB_LEN], char const *password) {
    size_t n = strlen(password);
    if (n > MF_PASSWORD_MAX) {
        return 0;
    }
    out[0] = MF_CMD_SEND_LOGIN;
    memcpy(out + 1, pub, MF_PUB_LEN);
    memcpy(out + 1 + MF_PUB_LEN, password, n);
    return 1 + MF_PUB_LEN + n;
}

size_t mf_encode_status_req(uint8_t *out, uint8_t const pub[MF_PUB_LEN]) {
    out[0] = MF_CMD_SEND_STATUS_REQ;
    memcpy(out + 1, pub, MF_PUB_LEN);
    return 1 + MF_PUB_LEN;
}

size_t mf_encode_telemetry_req(uint8_t *out, uint8_t const pub[MF_PUB_LEN]) {
    out[0] = MF_CMD_SEND_TELEMETRY_REQ;
    out[1] = out[2] = out[3] = 0; // reserved
    memcpy(out + 4, pub, MF_PUB_LEN);
    return 4 + MF_PUB_LEN;
}

size_t mf_encode_logout(uint8_t *out, uint8_t const pub[MF_PUB_LEN]) {
    out[0] = MF_CMD_LOGOUT;
    memcpy(out + 1, pub, MF_PUB_LEN);
    return 1 + MF_PUB_LEN;
}

size_t mf_encode_reset_path(uint8_t *out, uint8_t const pub[MF_PUB_LEN]) {
    out[0] = MF_CMD_RESET_PATH;
    memcpy(out + 1, pub, MF_PUB_LEN);
    return 1 + MF_PUB_LEN;
}

size_t mf_encode_remove_contact(uint8_t *out, uint8_t const pub[MF_PUB_LEN]) {
    out[0] = MF_CMD_REMOVE_CONTACT;
    memcpy(out + 1, pub, MF_PUB_LEN);
    return 1 + MF_PUB_LEN;
}

size_t mf_encode_trace(uint8_t *out, uint32_t tag, uint32_t auth, uint8_t flags, uint8_t const *hashes, size_t hash_len) {
    unsigned sz = flags & 3;
    if (hash_len == 0 || hash_len % (1u << sz) != 0 || (hash_len >> sz) > MF_PATH_MAX || 10 + hash_len > MF_MAX_FRAME) {
        return 0;
    }
    out[0] = MF_CMD_SEND_TRACE_PATH;
    put32(out + 1, tag);
    put32(out + 5, auth);
    out[9] = flags;
    memcpy(out + 10, hashes, hash_len);
    return 10 + hash_len;
}

size_t mf_encode_discovery(uint8_t *out, uint8_t const pub[MF_PUB_LEN]) {
    out[0] = MF_CMD_PATH_DISCOVERY;
    out[1] = 0; // reserved: MyMesh only takes 0
    memcpy(out + 2, pub, MF_PUB_LEN);
    return 2 + MF_PUB_LEN;
}

size_t mf_encode_update_contact(uint8_t *out, mf_contact_t const *c) {
    size_t i = 0;
    out[i++] = MF_CMD_ADD_UPDATE_CONTACT;
    memcpy(out + i, c->pub, MF_PUB_LEN);
    i += MF_PUB_LEN;
    out[i++] = c->type;
    out[i++] = c->flags;
    out[i++] = c->out_path_len;
    memcpy(out + i, c->out_path, MF_PATH_MAX);
    i += MF_PATH_MAX;
    memcpy(out + i, c->name, MF_NAME_MAX);
    i += MF_NAME_MAX;
    put32(out + i, c->last_advert_timestamp);
    put32(out + i + 4, (uint32_t)c->gps_lat);
    put32(out + i + 8, (uint32_t)c->gps_lon);
    put32(out + i + 12, c->lastmod);
    return i + 16;
}

// Shortest frame of each kind we read
static bool too_short(size_t len, size_t need) {
    return len < need;
}

static void decode_self_info(uint8_t const *f, size_t len, mf_msg_t *out) {
    if (too_short(len, 58)) {
        out->kind = MF_BAD;
        return;
    }
    out->kind = MF_SELF_INFO;
    memcpy(out->pub, f + 4, MF_PUB_LEN);
    size_t n = len - 58;
    if (n > MF_NAME_MAX - 1) {
        n = MF_NAME_MAX - 1;
    }
    memcpy(out->name, f + 58, n);
    out->name[n] = 0;
}

// Bytes of a packed path length (Packet::isValidPathLen): SIZE_MAX when it is not a valid one
static size_t path_bytes(uint8_t packed) {
    size_t count = packed & 63, size = (size_t)(packed >> 6) + 1;
    return size == 4 || count * size > MF_PATH_MAX ? SIZE_MAX : count * size;
}

// [0x89, 0, path_len, flags, tag, auth, hashes[path_len], snr[path_len >> sz], final snr]
static void decode_trace(uint8_t const *f, size_t len, mf_msg_t *out) {
    out->kind = MF_BAD;
    if (len < 13) {
        return;
    }
    mf_trace_t *t = &out->trace;
    t->hash_len   = f[2];
    t->flags      = f[3];
    t->hops       = (uint8_t)(t->hash_len >> (t->flags & 3));
    if (t->hash_len > MF_TRACE_HASH_MAX || t->hops > MF_PATH_MAX || len != 13u + t->hash_len + t->hops) {
        return;
    }
    t->tag  = get32(f + 4);
    t->auth = get32(f + 8);
    memcpy(t->hashes, f + 12, t->hash_len);
    memcpy(t->snr_q, f + 12 + t->hash_len, t->hops);
    t->final_snr_q = (int8_t)f[12 + t->hash_len + t->hops];
    out->kind      = MF_TRACE;
}

// [0x8D, 0, prefix[6], out_len, out path, in_len, in path]
static void decode_discovery(uint8_t const *f, size_t len, mf_msg_t *out) {
    out->kind = MF_BAD;
    if (len < 10) {
        return;
    }
    mf_discovery_t *d = &out->disc;
    memcpy(d->prefix, f + 2, 6);
    d->out_len   = f[8];
    size_t out_b = path_bytes(d->out_len);
    if (out_b == SIZE_MAX || len < 10 + out_b) {
        return;
    }
    memcpy(d->out_path, f + 9, out_b);
    d->in_len   = f[9 + out_b];
    size_t in_b = path_bytes(d->in_len);
    if (in_b == SIZE_MAX || len != 10 + out_b + in_b) {
        return;
    }
    memcpy(d->in_path, f + 10 + out_b, in_b);
    out->kind = MF_DISCOVERY;
}

static uint16_t get16(uint8_t const *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

// [0x85, admin, prefix6] (legacy) or [0x85, perms, prefix6, tag4, acl, fw_level]
static void decode_login_ok(uint8_t const *f, size_t len, mf_msg_t *out) {
    if (len < 8) {
        out->kind = MF_BAD;
        return;
    }
    out->kind  = MF_LOGIN_SUCCESS;
    out->admin = f[1] & 1;
    memcpy(out->rprefix, f + 2, MF_PREFIX);
    if (len >= 14) {
        out->acl      = f[12];
        out->fw_level = f[13];
    }
}

// [0x8B, 0, prefix6, Cayenne LPP]: the bytes are kept whole, mesh_telem.h reads them
static void decode_telemetry(uint8_t const *f, size_t len, mf_msg_t *out) {
    if (len < 8) {
        out->kind = MF_BAD;
        return;
    }
    out->kind = MF_TELEMETRY;
    memcpy(out->rprefix, f + 2, MF_PREFIX);
    size_t n       = len - 8 < MF_TELEM_MAX ? len - 8 : MF_TELEM_MAX;
    out->telem_len = (uint8_t)n;
    memcpy(out->telem, f + 8, n);
}

// [0x87, 0, prefix6, RepeaterStats]: fields are taken as far as the answer goes
static void decode_status(uint8_t const *f, size_t len, mf_msg_t *out) {
    if (len < 8) {
        out->kind = MF_BAD;
        return;
    }
    out->kind = MF_STATUS;
    memcpy(out->rprefix, f + 2, MF_PREFIX);
    uint8_t raw[MF_STATUS_BYTES] = {0};
    size_t  n                    = len - 8 < sizeof(raw) ? len - 8 : sizeof(raw);
    memcpy(raw, f + 8, n);
    mf_status_t *s = &out->status;
    s->bytes       = (uint8_t)n;
    s->batt_mv     = get16(raw);
    s->tx_queue    = get16(raw + 2);
    s->noise_floor = (int16_t)get16(raw + 4);
    s->last_rssi   = (int16_t)get16(raw + 6);
    s->n_recv      = get32(raw + 8);
    s->n_sent      = get32(raw + 12);
    s->air_secs    = get32(raw + 16);
    s->up_secs     = get32(raw + 20);
    s->sent_flood  = get32(raw + 24);
    s->sent_direct = get32(raw + 28);
    s->recv_flood  = get32(raw + 32);
    s->recv_direct = get32(raw + 36);
    s->err_events  = get16(raw + 40);
    s->last_snr_q  = (int16_t)get16(raw + 42);
    s->direct_dups = get16(raw + 44);
    s->flood_dups  = get16(raw + 46);
    s->rx_air_secs = get32(raw + 48);
    s->recv_errors = get32(raw + 52);
}

// RESP_CONTACT_MSG_RECV [7, prefix6, path_len, txt_type, ts4, (author4 if signed), text] and the V3 layout, which has
// [16, snr, 0, 0] in front. The text is cut to MF_MAX_TEXT and always terminated.
static void decode_contact_msg(uint8_t const *f, size_t len, mf_msg_t *out) {
    size_t at = f[0] == MF_RESP_CONTACT_MSG_RECV_V3 ? 4 : 1;
    if (len < at + 6 + 6) {
        return; // msg_ok stays false
    }
    memcpy(out->rprefix, f + at, MF_PREFIX);
    out->txt_type = f[at + 7];
    at += 12; // prefix, path_len, txt_type, timestamp
    if (out->txt_type == MF_TXT_SIGNED_PLAIN) {
        if (len < at + 4) {
            return;
        }
        memcpy(out->author, f + at, 4);
        at += 4;
    }
    size_t n = len - at < MF_MAX_TEXT ? len - at : MF_MAX_TEXT;
    memcpy(out->text, f + at, n);
    out->text[n] = 0;
    out->msg_ok  = true;
}

static void decode_err(uint8_t const *f, size_t len, mf_msg_t *out) {
    out->kind = too_short(len, 2) ? MF_BAD : MF_ERR;
    out->err  = len >= 2 ? f[1] : 0;
}

static void decode_sent(uint8_t const *f, size_t len, mf_msg_t *out) {
    if (too_short(len, 10)) {
        out->kind = MF_BAD;
        return;
    }
    out->kind        = MF_SENT;
    out->flood       = f[1];
    out->ack         = get32(f + 2);
    out->est_timeout = get32(f + 6);
}

static void decode_device_info(uint8_t const *f, size_t len, mf_msg_t *out) {
    out->kind   = too_short(len, 2) ? MF_BAD : MF_DEVICE_INFO;
    out->fw_ver = len >= 2 ? f[1] : 0;
}

// The four message codes: a channel message is only flagged (the mesh task reads it from MyMesh), a contact's is read
static void decode_msg_recv(uint8_t const *f, size_t len, mf_msg_t *out) {
    out->kind       = too_short(len, 7) ? MF_BAD : MF_MSG_RECV;
    out->is_channel = f[0] == MF_RESP_CHANNEL_MSG_RECV || f[0] == MF_RESP_CHANNEL_MSG_RECV_V3;
    if (out->kind == MF_MSG_RECV && !out->is_channel) {
        decode_contact_msg(f, len, out);
    }
}

static void decode_send_confirmed(uint8_t const *f, size_t len, mf_msg_t *out) {
    if (too_short(len, 9)) {
        out->kind = MF_BAD;
        return;
    }
    out->kind    = MF_SEND_CONFIRMED;
    out->ack     = get32(f + 1);
    out->trip_ms = get32(f + 5);
}

static void decode_login_fail(uint8_t const *f, size_t len, mf_msg_t *out) {
    out->kind = too_short(len, 8) ? MF_BAD : MF_LOGIN_FAIL;
    if (out->kind == MF_LOGIN_FAIL) {
        memcpy(out->rprefix, f + 2, MF_PREFIX);
    }
}

// Each code we read: a decoder that checks the frame's length itself, or (fn NULL) a kind with no payload. A code not
// listed is MF_UNKNOWN. A table rather than a switch keeps mf_decode one lookup (#212).
typedef void (*mf_decoder_t)(uint8_t const *f, size_t len, mf_msg_t *out);
static struct {
    uint8_t      code;
    uint8_t      kind; // mf_kind_t, when fn is NULL
    mf_decoder_t fn;
} const decoders[] = {
    {MF_RESP_OK, MF_OK, NULL},
    {MF_RESP_ERR, 0, decode_err},
    {MF_RESP_SELF_INFO, 0, decode_self_info},
    {MF_RESP_SENT, 0, decode_sent},
    {MF_RESP_NO_MORE_MESSAGES, MF_NO_MORE, NULL},
    {MF_RESP_DEVICE_INFO, 0, decode_device_info},
    {MF_RESP_CONTACT_MSG_RECV, 0, decode_msg_recv},
    {MF_RESP_CHANNEL_MSG_RECV, 0, decode_msg_recv},
    {MF_RESP_CONTACT_MSG_RECV_V3, 0, decode_msg_recv},
    {MF_RESP_CHANNEL_MSG_RECV_V3, 0, decode_msg_recv},
    {MF_PUSH_SEND_CONFIRMED, 0, decode_send_confirmed},
    {MF_PUSH_MSG_WAITING, MF_MSG_WAITING, NULL},
    {MF_PUSH_PATH_UPDATED, MF_PATH_UPDATED, NULL},
    {MF_PUSH_LOGIN_SUCCESS, 0, decode_login_ok},
    {MF_PUSH_LOGIN_FAIL, 0, decode_login_fail},
    {MF_PUSH_STATUS_RESPONSE, 0, decode_status},
    {MF_PUSH_TELEMETRY_RESPONSE, 0, decode_telemetry},
    {MF_PUSH_TRACE_DATA, 0, decode_trace},
    {MF_PUSH_PATH_DISCOVERY, 0, decode_discovery},
};

void mf_decode(uint8_t const *f, size_t len, mf_msg_t *out) {
    memset(out, 0, sizeof(*out));
    if (len == 0) {
        out->kind = MF_BAD;
        return;
    }
    out->code = f[0];
    out->kind = MF_UNKNOWN;
    for (size_t i = 0; i < sizeof(decoders) / sizeof(decoders[0]); i++) {
        if (decoders[i].code != f[0]) {
            continue;
        }
        if (decoders[i].fn) {
            decoders[i].fn(f, len, out);
        } else {
            out->kind = (mf_kind_t)decoders[i].kind;
        }
        return;
    }
}

size_t mf_text_cut(char const *text, size_t max) {
    size_t n = strlen(text);
    if (n <= max) {
        return n;
    }
    while (max > 0 && ((uint8_t)text[max] & 0xC0) == 0x80) {
        max--; // text[max] would be a continuation byte: the character starts earlier
    }
    return max;
}

size_t mf_channel_text_max(size_t node_name_len) {
    size_t prefix = node_name_len + 2; // "<name>: "
    return prefix >= MF_MAX_TEXT ? 0 : MF_MAX_TEXT - prefix;
}
