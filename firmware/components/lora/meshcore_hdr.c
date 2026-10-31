#include "meshcore_hdr.h"

#include <stdio.h>

#include <string.h>

// From meshcore/src/MeshCore.h and Packet.h
#define MAX_PACKET_PAYLOAD     184
#define MAX_PATH_SIZE          64
#define PAYLOAD_TYPE_ADVERT    4
// An advert's payload: public key, timestamp and signature before the app data
#define ADVERT_APP_DATA_OFFSET (32 + 4 + 64)
#define ADV_LATLON_MASK        0x10
#define ADV_FEAT1_MASK         0x20
#define ADV_FEAT2_MASK         0x40
#define ADV_NAME_MASK          0x80

static void parse_advert(uint8_t const *payload, size_t len, mc_hdr_t *out) {
    out->is_advert = true;
    if (len <= ADVERT_APP_DATA_OFFSET) {
        return;
    }
    uint8_t const *app   = payload + ADVERT_APP_DATA_OFFSET;
    size_t         alen  = len - ADVERT_APP_DATA_OFFSET;
    uint8_t        flags = app[0];
    out->adv_type        = flags & 0x0F;

    size_t i =
        1 + (flags & ADV_LATLON_MASK ? 8 : 0) + (flags & ADV_FEAT1_MASK ? 2 : 0) + (flags & ADV_FEAT2_MASK ? 2 : 0);
    if (!(flags & ADV_NAME_MASK) || i >= alen) {
        return;
    }
    size_t n = alen - i;
    if (n > sizeof(out->name) - 1) {
        n = sizeof(out->name) - 1;
    }
    for (size_t k = 0; k < n; k++) {
        uint8_t c    = app[i + k];
        out->name[k] = c >= 0x20 && c <= 0x7E ? (char)c : '?';
    }
    out->name[n] = '\0';
}

bool mc_hdr_parse(uint8_t const *pkt, size_t len, mc_hdr_t *out) {
    memset(out, 0, sizeof(*out));
    if (len < 2) {
        return false;
    }
    size_t  i  = 0;
    uint8_t h  = pkt[i++];
    out->route = h & 0x03;
    out->type  = (h >> 2) & 0x0F;
    out->ver   = (h >> 6) & 0x03;

    if (out->route == 0 || out->route == 3) {
        i += 4; // transport codes
    }
    if (i >= len) {
        return false;
    }
    uint8_t path_len     = pkt[i++];
    out->path_hash_count = path_len & 63;
    out->path_hash_size  = (path_len >> 6) + 1;
    size_t path_bytes    = (size_t)out->path_hash_count * out->path_hash_size;
    if (out->path_hash_size == 4 || path_bytes > MAX_PATH_SIZE) {
        return false;
    }
    if ((out->route == 0 || out->route == 1) && out->path_hash_count && i + path_bytes <= len) {
        memcpy(out->last_hop, pkt + i + path_bytes - out->path_hash_size, out->path_hash_size);
    }
    i += path_bytes;
    if (i >= len || len - i > MAX_PACKET_PAYLOAD) {
        return false;
    }
    out->payload_len = (uint8_t)(len - i);

    if (out->type == PAYLOAD_TYPE_ADVERT) {
        parse_advert(pkt + i, out->payload_len, out);
    }
    return true;
}

size_t mc_hdr_format(mc_hdr_t const *h, char *out, size_t cap) {
    static char const *const routes[] = {"tflood", "flood", "direct", "tdirect"};
    static char const *const types[]  = {
        "req",
        "resp",
        "txt",
        "ack",
        "advert",
        "grp_txt",
        "grp_data",
        "anon_req",
        "path",
        "trace",
        "multipart",
        "control"
    };
    static char const *const advs[] = {"none", "chat", "repeater", "room", "sensor"};

    if (!cap) {
        return 0;
    }
    char        type_buf[12];
    char const *type = type_buf;
    if (h->type < sizeof(types) / sizeof(types[0])) {
        type = types[h->type];
    } else if (h->type == 0x0F) {
        type = "raw";
    } else {
        snprintf(type_buf, sizeof(type_buf), "type%u", h->type);
    }

    int n = snprintf(
        out,
        cap,
        "%s %s v%u path %ux%u",
        routes[h->route & 3],
        type,
        h->ver,
        h->path_hash_count,
        h->path_hash_size
    );
    // Each repeater appends its hash to a flood packet's path, so the last one is who we heard it from
    if (n >= 0 && (size_t)n < cap && (h->route == 0 || h->route == 1) && h->path_hash_count) {
        int m = snprintf(out + n, cap - (size_t)n, " via ");
        n     = m < 0 ? n : n + m;
        for (uint8_t k = 0; k < h->path_hash_size && n >= 0 && (size_t)n < cap; k++) {
            m = snprintf(out + n, cap - (size_t)n, "%02x", h->last_hop[k]);
            n = m < 0 ? n : n + m;
        }
    }
    if (n >= 0 && (size_t)n < cap && h->is_advert && h->name[0]) {
        char        adv_buf[12];
        char const *adv = adv_buf;
        if (h->adv_type < sizeof(advs) / sizeof(advs[0])) {
            adv = advs[h->adv_type];
        } else {
            snprintf(adv_buf, sizeof(adv_buf), "adv%u", h->adv_type);
        }
        int m = snprintf(out + n, cap - (size_t)n, " '%s' (%s)", h->name, adv);
        n     = m < 0 ? n : n + m;
    }
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n < cap ? (size_t)n : cap - 1;
}
