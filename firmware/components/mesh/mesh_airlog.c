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

// The hop count of a path_len byte (src/Packet.h: hash size - 1 in the top two bits, the count below), 0 for one the
// header cannot hold (hash size 4, more than MAX_PATH_BYTES)
static uint8_t path_hops(uint8_t path_len, uint8_t *hash_size) {
    uint8_t size  = (uint8_t)((path_len >> 6) + 1);
    uint8_t count = path_len & 63;
    *hash_size    = size;
    return size <= 3 && (size_t)count * size <= MAX_PATH_BYTES ? count : 0;
}

static void ring_put(airlog_t *r, airlog_entry_t *e, uint8_t header) {
    e->seq        = r->seq + 1;
    e->route      = header & 0x03;
    e->type       = (header >> 2) & 0x0F;
    r->e[r->next] = *e;
    r->next       = (r->next + 1) % r->cap;
    r->count      = r->count < r->cap ? r->count + 1 : r->cap;
    r->seq        = e->seq;
}

void airlog_push(airlog_t *r, uint8_t header, uint8_t path_len, uint8_t const *path, uint8_t raw_len, int16_t rssi_x10,
                 int16_t snr_x10, uint32_t at_s) {
    if (!r->e || r->cap == 0) {
        return;
    }
    airlog_entry_t e = {.at_s = at_s, .rssi_x10 = rssi_x10, .snr_x10 = snr_x10, .len = raw_len};
    uint8_t        size;
    uint8_t        count = path_hops(path_len, &size);
    if (path && count) {
        e.hops      = count;
        e.hash_size = size;
        bool flood  = (header & 0x03) <= 1;
        memcpy(e.hash, path + (flood ? (size_t)(count - 1) * size : 0), size);
    }
    ring_put(r, &e, header);
}

// Only fixed-size stores into the preallocated ring: it runs on the mesh task right after a transmit (#408)
void airlog_push_tx(airlog_t *r, uint8_t header, uint8_t path_len, uint8_t raw_len, bool retry,
                    airlog_tx_result_t result, uint32_t at_s) {
    if (!r->e || r->cap == 0) {
        return;
    }
    uint8_t        size;
    airlog_entry_t e = {.at_s = at_s, .len = raw_len, .result = (uint8_t)result};
    e.hops           = path_hops(path_len, &size);
    e.flags          = (uint8_t)(AIRLOG_F_SENT | (retry ? AIRLOG_F_RETRY : 0));
    ring_put(r, &e, header);
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

bool airlog_is_sent(airlog_entry_t const *e) {
    return (e->flags & AIRLOG_F_SENT) != 0;
}

static char const *const TX_RESULTS[] = {"ok", "failed", "timeout", "refused"};

static char const *tx_result(uint8_t result) {
    return result < sizeof(TX_RESULTS) / sizeof(TX_RESULTS[0]) ? TX_RESULTS[result] : "unknown";
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

static void sent_detail(airlog_entry_t const *e, char const *age_text, char *out, size_t cap) {
    char const *retry = e->flags & AIRLOG_F_RETRY ? ", retry" : "";
    if (e->result == AIRLOG_TX_OK) {
        snprintf(out, cap, "Sent%s, %s", retry, age_text);
    } else {
        snprintf(out, cap, "Sent%s, %s, %s", retry, tx_result(e->result), age_text);
    }
}

void airlog_detail(airlog_entry_t const *e, char const *age_text, char *out, size_t cap) {
    if (airlog_is_sent(e)) {
        sent_detail(e, age_text, out, cap);
        return;
    }
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

char const *airlog_kind_token(uint8_t type) {
    static char const *const TOKENS[] = {
        "req", "resp", "txt", "ack", "advert", "grp_txt", "grp_data", "anon_req", "path", "trace", "multipart", "control",
    };
    static char other[12];
    if (type < sizeof(TOKENS) / sizeof(TOKENS[0])) {
        return TOKENS[type];
    }
    snprintf(other, sizeof(other), "type%u", (unsigned)type);
    return other;
}

// A tenth as "-87.4" / "6.5" / "-0.5": the sign separate, so -5 is not printed as "0.5"
static void fmt_tenths(int16_t v, char *out, size_t cap) {
    int a = v < 0 ? -(int)v : v;
    snprintf(out, cap, "%s%d.%d", v < 0 ? "-" : "", a / 10, a % 10);
}

void airlog_console_line(airlog_entry_t const *e, char *out, size_t cap) {
    char const *route = airlog_is_flood(e) ? "flood" : "direct";
    if (airlog_is_sent(e)) {
        snprintf(out, cap, "#%lu t=%lu tx %s %s hops=%u len=%u retry=%u result=%s", (unsigned long)e->seq,
                 (unsigned long)e->at_s, airlog_kind_token(e->type), route, (unsigned)e->hops, (unsigned)e->len,
                 (e->flags & AIRLOG_F_RETRY) ? 1u : 0u, tx_result(e->result));
        return;
    }
    char rssi[12], snr[12];
    fmt_tenths(e->rssi_x10, rssi, sizeof(rssi));
    fmt_tenths(e->snr_x10, snr, sizeof(snr));
    snprintf(out, cap, "#%lu t=%lu rx %s %s hops=%u len=%u rssi=%s snr=%s", (unsigned long)e->seq,
             (unsigned long)e->at_s, airlog_kind_token(e->type), route, (unsigned)e->hops, (unsigned)e->len, rssi, snr);
}

void airlog_console_header(size_t shown, uint32_t seq, char *out, size_t cap) {
    if (shown == 0) {
        snprintf(out, cap, "air log: empty");
        return;
    }
    snprintf(out, cap, "air log: %u entr%s, newest first, %lu logged since boot", (unsigned)shown,
             shown == 1 ? "y" : "ies", (unsigned long)seq);
}
