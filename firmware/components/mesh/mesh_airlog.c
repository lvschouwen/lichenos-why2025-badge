#include "mesh_airlog.h"

#include "mesh_health.h"

#include <stdio.h>
#include <string.h>

#define MAX_PATH_BYTES 64 // meshcore/src/MeshCore.h MAX_PATH_SIZE

void airlog_init(airlog_t *r, airlog_entry_t *storage, size_t cap) {
    r->e     = storage;
    r->cap   = cap;
    r->next  = 0;
    r->count = 0;
    r->seq   = 0;
}

void airlog_push(airlog_t *r, uint8_t header, uint8_t path_len, uint8_t const *path, uint8_t raw_len, int16_t rssi_x10,
                 int16_t snr_x10, uint32_t at_s) {
    if (!r->e || r->cap == 0) {
        return;
    }
    airlog_entry_t e = {.seq = r->seq + 1, .at_s = at_s, .rssi_x10 = rssi_x10, .snr_x10 = snr_x10, .len = raw_len};
    e.route          = header & 0x03;
    e.type           = (header >> 2) & 0x0F;
    uint8_t size     = (uint8_t)((path_len >> 6) + 1);
    uint8_t count    = path_len & 63;
    if (size <= 3 && (size_t)count * size <= MAX_PATH_BYTES && path && count) {
        e.hops      = count;
        e.hash_size = size;
        bool flood  = e.route <= 1;
        memcpy(e.hash, path + (flood ? (size_t)(count - 1) * size : 0), size);
    }
    r->e[r->next] = e;
    r->next       = (r->next + 1) % r->cap;
    r->count      = r->count < r->cap ? r->count + 1 : r->cap;
    r->seq        = e.seq;
}

size_t airlog_copy(airlog_t const *r, airlog_entry_t *out, size_t max) {
    size_t n = r->count < max ? r->count : max;
    for (size_t i = 0; i < n; i++) {
        out[i] = r->e[(r->next + r->cap - 1 - i) % r->cap];
    }
    return n;
}

bool airlog_is_flood(airlog_entry_t const *e) {
    return e->route <= 1;
}

char const *airlog_type_name(uint8_t type) {
    static char const *const NAMES[] = {
        "Request", "Response", "Direct message", "Ack", "Advert", "Channel message", "Channel data", "Anonymous request",
        "Path", "Trace", "Multipart", "Control",
    };
    static char other[12];
    if (type < sizeof(NAMES) / sizeof(NAMES[0])) {
        return NAMES[type];
    }
    snprintf(other, sizeof(other), "Type %u", (unsigned)type);
    return other;
}

void airlog_title(airlog_entry_t const *e, char *out, size_t cap) {
    char hops[16];
    if (e->hops == 0) {
        snprintf(hops, sizeof(hops), "no hops");
    } else {
        snprintf(hops, sizeof(hops), "%u hop%s", (unsigned)e->hops, e->hops == 1 ? "" : "s");
    }
    snprintf(out, cap, "%s, %s, %s", airlog_type_name(e->type), airlog_is_flood(e) ? "flood" : "direct", hops);
}

void airlog_detail(airlog_entry_t const *e, char const *age_text, char *out, size_t cap) {
    char rssi[16], snr[16], via[16] = "";
    mh_fmt_dbm(e->rssi_x10, rssi, sizeof(rssi));
    mh_fmt_db(e->snr_x10, snr, sizeof(snr));
    if (e->hash_size) {
        size_t n = (size_t)snprintf(via, sizeof(via), "%s ", airlog_is_flood(e) ? "via" : "next");
        for (uint8_t i = 0; i < e->hash_size && n + 3 < sizeof(via); i++) {
            n += (size_t)snprintf(via + n, sizeof(via) - n, "%02x", e->hash[i]);
        }
    }
    snprintf(out, cap, "%s, %s SNR%s%s, %s", rssi, snr, via[0] ? ", " : "", via, age_text);
}
