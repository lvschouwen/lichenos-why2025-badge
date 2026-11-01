// Sharing channels and contacts as text (#54): see mesh_share.h
#include "mesh_share.h"

#include "mesh_contact_fmt.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define SCHEME          "meshcore://"
#define SCHEME_LEN      (sizeof(SCHEME) - 1)
#define PH_ROUTE_MASK   0x03
#define ROUTE_FLOOD     0x01
#define PAYLOAD_ADVERT  0x04
#define MAX_PATH_BYTES  64
#define MAX_PAYLOAD     184
#define ADVERT_MIN      (32 + 4 + 64) // public key, timestamp, signature
#define CHANNEL_KEY_STD 16

size_t mesh_channel_key_len(uint8_t const secret[MESH_CHANNEL_SECRET]) {
    for (size_t i = CHANNEL_KEY_STD; i < MESH_CHANNEL_SECRET; i++) {
        if (secret[i]) {
            return MESH_CHANNEL_SECRET;
        }
    }
    return CHANNEL_KEY_STD;
}

bool mesh_hex_text(uint8_t const *bytes, size_t n, char *out, size_t cap) {
    static char const digits[] = "0123456789abcdef";
    if (cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (2 * n + 1 > cap) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * n] = '\0';
    return true;
}

bool mesh_base64_text(uint8_t const *bytes, size_t n, char *out, size_t cap) {
    static char const abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (cap == 0) {
        return false;
    }
    out[0] = '\0';
    if ((n + 2) / 3 * 4 + 1 > cap) {
        return false;
    }
    size_t at = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)bytes[i] << 16;
        v |= i + 1 < n ? (uint32_t)bytes[i + 1] << 8 : 0;
        v |= i + 2 < n ? bytes[i + 2] : 0;
        out[at++] = abc[v >> 18 & 63];
        out[at++] = abc[v >> 12 & 63];
        out[at++] = i + 1 < n ? abc[v >> 6 & 63] : '=';
        out[at++] = i + 2 < n ? abc[v & 63] : '=';
    }
    out[at] = '\0';
    return true;
}

bool mesh_channel_uri(char const *name, uint8_t const *secret, size_t key_len, char *out, size_t cap) {
    char enc[31 * 3 + 1], hex[CHANNEL_KEY_STD * 2 + 1];
    if (cap) {
        out[0] = '\0';
    }
    if (key_len != CHANNEL_KEY_STD || !name || !name[0] || mesh_url_encode(name, enc, sizeof(enc)) == SIZE_MAX) {
        return false;
    }
    mesh_hex_text(secret, key_len, hex, sizeof(hex));
    int n = snprintf(out, cap, SCHEME "channel/add?name=%s&secret=%s", enc, hex);
    if (n < 0 || (size_t)n >= cap) {
        if (cap) {
            out[0] = '\0';
        }
        return false;
    }
    return true;
}

size_t mesh_advert_normalize(uint8_t const *in, size_t len, uint8_t *out, size_t cap) {
    if (len < 2) {
        return 0;
    }
    uint8_t header = in[0];
    if ((header >> 2 & 0x0F) != PAYLOAD_ADVERT) {
        return 0;
    }
    uint8_t route = header & PH_ROUTE_MASK;
    size_t  at    = 1 + (route == 0x00 || route == 0x03 ? 4 : 0); // transport codes
    if (at >= len) {
        return 0;
    }
    uint8_t path_len  = in[at++];
    size_t  hash_size = (size_t)(path_len >> 6) + 1;
    size_t  path      = (size_t)(path_len & 63) * hash_size;
    if (hash_size == 4 || path > MAX_PATH_BYTES) {
        return 0; // as Packet::isValidPathLen
    }
    at += path;
    if (at > len) {
        return 0;
    }
    size_t payload = len - at;
    if (payload < ADVERT_MIN || payload > MAX_PAYLOAD || 2 + payload > cap) {
        return 0;
    }
    memmove(out + 2, in + at, payload); // in and out may be the same buffer: the payload only moves down
    out[0] = (uint8_t)((header & ~PH_ROUTE_MASK) | ROUTE_FLOOD);
    out[1] = 0;
    return 2 + payload;
}

bool mesh_advert_uri(uint8_t const *pkt, size_t len, char *out, size_t cap) {
    uint8_t norm[MESH_ADVERT_MAX];
    if (cap) {
        out[0] = '\0';
    }
    size_t n = len <= sizeof(norm) ? mesh_advert_normalize(pkt, len, norm, sizeof(norm)) : 0;
    if (!n || cap < SCHEME_LEN + 1) {
        return false;
    }
    memcpy(out, SCHEME, SCHEME_LEN);
    if (!mesh_hex_text(norm, n, out + SCHEME_LEN, cap - SCHEME_LEN)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

size_t mesh_advert_uri_parse(char const *text, uint8_t *out, size_t cap, char const **why) {
    char const *local;
    why  = why ? why : &local;
    *why = NULL;
    while (*text == ' ') {
        text++;
    }
    size_t len = strlen(text);
    while (len && text[len - 1] == ' ') {
        len--;
    }
    if (len < SCHEME_LEN || strncasecmp(text, SCHEME, SCHEME_LEN) != 0) {
        *why = "not a meshcore:// link";
        return 0;
    }
    text += SCHEME_LEN, len -= SCHEME_LEN;
    if (strncasecmp(text, "contact/add?", 12) == 0 || strncasecmp(text, "channel/add?", 12) == 0) {
        *why = "that is an app link (name and key), not an exported contact: add it in the MeshCore app";
        return 0;
    }
    uint8_t raw[MESH_ADVERT_MAX];
    if (len % 2 || len / 2 > sizeof(raw)) {
        *why = len % 2 ? "odd number of hex digits (a part missing?)" : "too long for a contact";
        return 0;
    }
    for (size_t i = 0; i < len / 2; i++) {
        int hi = hex_value(text[2 * i]), lo = hex_value(text[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            *why = "not hex after meshcore://";
            return 0;
        }
        raw[i] = (uint8_t)(hi << 4 | lo);
    }
    size_t n = mesh_advert_normalize(raw, len / 2, out, cap);
    if (!n) {
        *why = "not a MeshCore contact (advert) packet";
    }
    return n;
}

char const *mesh_import_add(mesh_import_t *st, char const *part) {
    while (*part == ' ') {
        part++;
    }
    bool start = strncasecmp(part, SCHEME, SCHEME_LEN) == 0;
    if (start) {
        st->len = 0;
    } else if (st->len == 0) {
        st->text[0] = '\0';
        return "start with the part that begins meshcore://";
    }
    for (; *part; part++) {
        if (*part == ' ') {
            continue;
        }
        if (st->len + 1 >= sizeof(st->text)) {
            st->len     = 0;
            st->text[0] = '\0';
            return "too long for a contact link: dropped, start again";
        }
        st->text[st->len++] = *part;
    }
    st->text[st->len] = '\0';
    return NULL;
}
