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

size_t mf_encode_reset_path(uint8_t *out, uint8_t const pub[MF_PUB_LEN]) {
    out[0] = MF_CMD_RESET_PATH;
    memcpy(out + 1, pub, MF_PUB_LEN);
    return 1 + MF_PUB_LEN;
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

void mf_decode(uint8_t const *f, size_t len, mf_msg_t *out) {
    memset(out, 0, sizeof(*out));
    if (len == 0) {
        out->kind = MF_BAD;
        return;
    }
    out->code = f[0];
    out->kind = MF_UNKNOWN;
    switch (f[0]) {
    case MF_RESP_OK:
        out->kind = MF_OK;
        break;
    case MF_RESP_ERR:
        out->kind = too_short(len, 2) ? MF_BAD : MF_ERR;
        out->err  = len >= 2 ? f[1] : 0;
        break;
    case MF_RESP_SELF_INFO:
        decode_self_info(f, len, out);
        break;
    case MF_RESP_SENT:
        if (too_short(len, 10)) {
            out->kind = MF_BAD;
            break;
        }
        out->kind        = MF_SENT;
        out->flood       = f[1];
        out->ack         = get32(f + 2);
        out->est_timeout = get32(f + 6);
        break;
    case MF_RESP_NO_MORE_MESSAGES:
        out->kind = MF_NO_MORE;
        break;
    case MF_RESP_DEVICE_INFO:
        out->kind   = too_short(len, 2) ? MF_BAD : MF_DEVICE_INFO;
        out->fw_ver = len >= 2 ? f[1] : 0;
        break;
    case MF_RESP_CONTACT_MSG_RECV:
    case MF_RESP_CHANNEL_MSG_RECV:
    case MF_RESP_CONTACT_MSG_RECV_V3:
    case MF_RESP_CHANNEL_MSG_RECV_V3:
        out->kind       = too_short(len, 7) ? MF_BAD : MF_MSG_RECV;
        out->is_channel = f[0] == MF_RESP_CHANNEL_MSG_RECV || f[0] == MF_RESP_CHANNEL_MSG_RECV_V3;
        break;
    case MF_PUSH_SEND_CONFIRMED:
        if (too_short(len, 9)) {
            out->kind = MF_BAD;
            break;
        }
        out->kind    = MF_SEND_CONFIRMED;
        out->ack     = get32(f + 1);
        out->trip_ms = get32(f + 5);
        break;
    case MF_PUSH_MSG_WAITING:
        out->kind = MF_MSG_WAITING;
        break;
    case MF_PUSH_PATH_UPDATED:
        out->kind = MF_PATH_UPDATED;
        break;
    case MF_PUSH_LOGIN_SUCCESS:
        out->kind = MF_LOGIN_SUCCESS;
        break;
    case MF_PUSH_LOGIN_FAIL:
        out->kind = MF_LOGIN_FAIL;
        break;
    case MF_PUSH_STATUS_RESPONSE:
        out->kind = MF_STATUS;
        break;
    default:
        break;
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
