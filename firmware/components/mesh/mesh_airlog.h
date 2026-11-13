#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The air log (#165): a ring of the packets MeshCore received and (#408) transmitted, newest last pushed, headers only.
// Nothing of the payload is kept (no message text, no names, no bytes): type, route, hops, signal and time; a received
// packet also keeps the last hop's hash, a sent one keeps none (its console lines are quoted in public issues). RAM
// only. Pure logic, no ESP headers: built into the host tests. The ring is not locked; the caller serialises.

#define AIRLOG_CAP 200

#define AIRLOG_F_SENT  0x01 // transmitted by this badge (rssi, snr and hash unused)
#define AIRLOG_F_RETRY 0x02 // sent: the outbox's resend of a direct message (its attempt 2 or 3)

// How a transmit ended, as MeshCore's dispatcher saw it: each packet it starts ends in exactly one of these. A packet
// too long for the radio is dropped by the dispatcher before it starts (nothing goes on air) and gets no entry.
typedef enum {
    AIRLOG_TX_OK,      // the C6 reported it sent (the "tx" count of "fw mesh")
    AIRLOG_TX_FAILED,  // the C6 reported it failed ("failed")
    AIRLOG_TX_TIMEOUT, // MeshCore gave up before the C6 answered ("timed out")
    // Never started: the C6 still busy with the previous packet, which is all the "refused" count of "fw mesh"
    // counts, but also TX blocked (a damaged identity), LoRa switched off or an empty packet. Scripts must not equate
    // the two.
    AIRLOG_TX_REFUSED,
} airlog_tx_result_t;

// The result of a packet's end: the radio's last outcome (BadgeRadio::lastTxResult), and whether it ended in logTx
// (completed: the C6 answered) rather than logTxFail, which is never a send
static inline airlog_tx_result_t airlog_tx_outcome(uint8_t radio_last, bool completed) {
    if (radio_last > AIRLOG_TX_REFUSED) {
        return completed ? AIRLOG_TX_OK : AIRLOG_TX_TIMEOUT;
    }
    airlog_tx_result_t r = (airlog_tx_result_t)radio_last;
    if (!completed && (r == AIRLOG_TX_OK || r == AIRLOG_TX_FAILED)) {
        return AIRLOG_TX_TIMEOUT;
    }
    return r;
}

// Whether the ring exists, and how many notes (received or sent) were dropped because it did not
typedef struct {
    bool     have_ring;
    uint32_t dropped;
} airlog_info_t;

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
    uint8_t  flags;       // AIRLOG_F_*
    uint8_t  result;      // sent: airlog_tx_result_t
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
// A packet this badge transmitted (#408): header and path_len as for airlog_push, raw_len the bytes on air. Only the
// path's hop count is kept, never its hashes.
void airlog_push_tx(airlog_t *r, uint8_t header, uint8_t path_len, uint8_t raw_len, bool retry,
                    airlog_tx_result_t result, uint32_t at_s);
// Up to max entries, newest first; returns how many
size_t airlog_copy(airlog_t const *r, airlog_entry_t *out, size_t max);

bool airlog_is_flood(airlog_entry_t const *e);
bool airlog_is_sent(airlog_entry_t const *e);
// "Advert" / "Channel message" / "Direct message" ... a name for the payload type; "Type 12" for one without a name
char const *airlog_type_name(uint8_t type);
// The row's first line: "Advert, flood, 3 hops" / "Ack, direct, no hops"
void airlog_title(airlog_entry_t const *e, char *out, size_t cap);
// The second line: "-87 dBm, 6.5 dB SNR, via 1f2e, 3 min ago"; a sent one "Sent, 3 min ago" ("Sent, retry, failed,
// ..."); age_text is the caller's ("3 min ago")
void airlog_detail(airlog_entry_t const *e, char const *age_text, char *out, size_t cap);

// The console's form ("fw mesh airlog", #408), a contract for bench scripts (host_tests/mesh_airlog_test.c pins it):
// numbers and enums only, one entry per line.
//   rx: "#57 t=1234 rx advert flood hops=3 len=90 rssi=-87.4 snr=6.5"
//   tx: "#58 t=1240 tx txt direct hops=1 len=45 retry=1 result=ok"
// The kind tokens: req resp txt ack advert grp_txt grp_data anon_req (a login) path trace multipart control, else
// type<n> (written into buf, which needs AIRLOG_KIND_MAX bytes); the results: ok failed timeout refused.
#define AIRLOG_KIND_MAX 12
char const *airlog_kind_token(uint8_t type, char *buf, size_t cap);
void        airlog_console_line(airlog_entry_t const *e, char *out, size_t cap);
// The line before them, always with the notes dropped for lack of a ring:
//   "air log: 2 entries, newest first, 58 logged since boot, 0 dropped" / "air log: empty, 0 dropped" /
//   "air log: no memory, 7 dropped" (the ring could not be allocated: nothing was recorded, not "nothing happened")
void        airlog_console_header(size_t shown, uint32_t seq, airlog_info_t const *info, char *out, size_t cap);

// "fw mesh airlog [n]": n from 1 to AIRLOG_CONSOLE_MAX, AIRLOG_CONSOLE_DEFAULT when arg is empty or blank; spaces
// around the number are fine. false for anything else.
#define AIRLOG_CONSOLE_DEFAULT 20
#define AIRLOG_CONSOLE_MAX     50 // bounded output; the ring keeps AIRLOG_CAP
bool        airlog_console_count(char const *arg, unsigned *n);

#ifdef __cplusplus
}
#endif
