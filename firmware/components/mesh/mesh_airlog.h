#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The air log (#165): a ring of the packets MeshCore received, newest last pushed, headers only. Nothing of the
// payload is kept (no message text, no names, no bytes): type, route, hops, the last hop's hash, signal and time. RAM
// only. Pure logic, no ESP headers: built into the host tests. The ring is not locked; the caller serialises.

#define AIRLOG_CAP 200

typedef struct {
    uint32_t seq;         // 1, 2, 3 ... as received: a stable identity for a row
    uint32_t at_s;        // seconds since boot
    int16_t  rssi_x10;    // dBm * 10
    int16_t  snr_x10;     // dB * 10
    uint8_t  type;        // PAYLOAD_TYPE_*
    uint8_t  route;       // ROUTE_TYPE_*: 0 transport flood, 1 flood, 2 direct, 3 transport direct
    uint8_t  hops;        // hashes in the path
    uint8_t  hash_size;   // bytes per hash (1..3), 0 when the path is empty
    uint8_t  hash[3];     // flood: the repeater heard last (the end of the path); direct: the next hop (its start)
    uint8_t  len;         // bytes on air
} airlog_entry_t;

typedef struct {
    airlog_entry_t *e;
    size_t          cap;
    size_t          next;  // where the next entry goes
    size_t          count; // entries held, up to cap
    uint32_t        seq;   // sequence number of the newest
} airlog_t;

void airlog_init(airlog_t *r, airlog_entry_t *storage, size_t cap);
// header and path_len as MeshCore's packet carries them (src/Packet.h), path the bytes of the path; raw_len the bytes
// on air. An invalid path_len (hash size 4, more than 64 path bytes) records a packet with no path.
void airlog_push(airlog_t *r, uint8_t header, uint8_t path_len, uint8_t const *path, uint8_t raw_len, int16_t rssi_x10,
                 int16_t snr_x10, uint32_t at_s);
// Up to max entries, newest first; returns how many
size_t airlog_copy(airlog_t const *r, airlog_entry_t *out, size_t max);

bool airlog_is_flood(airlog_entry_t const *e);
// "Advert" / "Channel message" / "Direct message" ... a name for the payload type; "Type 12" for one without a name
char const *airlog_type_name(uint8_t type);
// The row's first line: "Advert, flood, 3 hops" / "Ack, direct, no hops"
void airlog_title(airlog_entry_t const *e, char *out, size_t cap);
// The second line: "-87 dBm, 6.5 dB SNR, via 1f2e, 3 min ago"; age_text is the caller's ("3 min ago")
void airlog_detail(airlog_entry_t const *e, char const *age_text, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
