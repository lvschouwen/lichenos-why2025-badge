// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
// NOT LINKED: neither the P4 nor the C6 image includes this yet; only host_tests/i154_proto_test builds it. It is
// pre-work for the C6 802.15.4 plan (#234); keep it until that lands.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../lora/lora_proto.h" // the LoRa ids the i154 ids must differ from

#ifdef __cplusplus
extern "C" {
#endif

// The 802.15.4 scanner (#172): messages between the P4 and the C6's scan service over ESP-Hosted peer data, an
// 802.15.4 MAC header parser, a per-network dedupe table and the P4's "does this C6 scan at all" probe.
// Pure logic, no ESP headers: built into the C6 image, the P4 firmware and the host tests.
//
// Every message starts with [version][type]; the fields follow little-endian in the order of the structs below, and
// every message has one fixed length. A layout change bumps I154_PROTO_VERSION.
//
// Frozen across versions, so the two sides can always tell each other apart: CAPS_REQ is [version][1] and CAPS is
// [version][0x81][modes][channel_mask u32][table_size], to which a later version may only append. The C6 answers
// every CAPS_REQ (i154_is_caps_req, whatever its version) with a CAPS in its own version; the P4 reads the CAPS prefix
// of any version (i154_probe_on_msg).
//
// Exchange: CAPS_REQ -> CAPS. SCAN_START -> ACK within I154_SCAN_ACK_MS (or DONE when refused), then ED and FRAME
// messages, then one DONE. SCAN_STOP -> DONE (STOPPED).
//
// Left for the C6 service (#172 2E):
// - the parser trusts the radio's FCS check (no software CRC); the service strips the PHY length byte and the 2-byte
//   FCS before i154_mac_parse, and must not underflow on a length of 0 or 1 (pass 0: the parser returns SHORT);
// - licence housekeeping once shared/i154 is linked: shared/i154 in tools/third_party_licenses.py, and
//   CLAUDE.md / docs/architecture.md naming shared/i154 (Apache-2.0) and the ids 0x4C03/0x4C04.

#define I154_PROTO_VERSION 1
#define I154_MSG_ID_TO_C6  0x4C03 // next to LORA_MSG_ID_TO_C6 0x4C01 / TO_P4 0x4C02
#define I154_MSG_ID_TO_P4  0x4C04
// A shared id would route one service's messages into the other's callback
#ifdef __cplusplus
static_assert(I154_MSG_ID_TO_C6 != LORA_MSG_ID_TO_C6 && I154_MSG_ID_TO_C6 != LORA_MSG_ID_TO_P4 &&
                  I154_MSG_ID_TO_P4 != LORA_MSG_ID_TO_C6 && I154_MSG_ID_TO_P4 != LORA_MSG_ID_TO_P4 &&
                  I154_MSG_ID_TO_C6 != I154_MSG_ID_TO_P4,
              "peer-data ids must be unique");
#else
_Static_assert(I154_MSG_ID_TO_C6 != LORA_MSG_ID_TO_C6 && I154_MSG_ID_TO_C6 != LORA_MSG_ID_TO_P4 &&
                   I154_MSG_ID_TO_P4 != LORA_MSG_ID_TO_C6 && I154_MSG_ID_TO_P4 != LORA_MSG_ID_TO_P4 &&
                   I154_MSG_ID_TO_C6 != I154_MSG_ID_TO_P4,
               "peer-data ids must be unique");
#endif
#define I154_MSG_MAX       64 // largest encoded message (FRAME is 50); CAPS from a newer C6 may be longer

#define I154_CHANNEL_MIN      11
#define I154_CHANNEL_MAX      26
#define I154_CHANNEL_MASK_ALL 0x07FFF800u // bit n = channel n, 11..26 (2.4 GHz O-QPSK)

typedef enum {
    // P4 -> C6
    I154_MSG_CAPS_REQ = 1, // does the C6 scan? Answered by CAPS. An older C6 has no handler and answers nothing
    I154_MSG_SCAN_START,   // answered by ED/FRAME messages and one DONE
    I154_MSG_SCAN_STOP,    // ends the running scan early: DONE with I154_DONE_STOPPED
    // C6 -> P4
    I154_MSG_CAPS = 0x81,
    I154_MSG_ED,    // energy on one channel
    I154_MSG_FRAME, // a network (channel + PAN) the scan heard, sent when it is new or its summary changed
    I154_MSG_DONE,  // the scan ended, or was refused
    I154_MSG_ACK,   // SCAN_START accepted: sent before the first channel, so a lost SCAN_START shows within seconds
} i154_msg_type_t;

// Passive and active also measure the energy on each channel first: passive + ED is the default (Lucas, 2026-11-07).
// Active transmits a beacon request on every channel, so the P4 sends it only after the user confirmed
typedef enum {
    I154_MODE_ED = 1,  // energy detect only: dwell_ms is the measurement time per channel
    I154_MODE_PASSIVE, // listen only, never transmits: dwell_ms of listening per channel
    I154_MODE_ACTIVE,  // one beacon request per channel, then listen for dwell_ms
} i154_mode_t;

#define I154_MODE_BIT(m) (1u << (m))

#define I154_ED_DWELL_MIN_MS      1
#define I154_ED_DWELL_MAX_MS      50
#define I154_LISTEN_DWELL_MIN_MS  100
#define I154_PASSIVE_DWELL_MAX_MS 30000  // a Zigbee router sends link status about every 15 s
#define I154_ACTIVE_DWELL_MAX_MS  5000   // beacons answer within a few superframes
#define I154_SCAN_MAX_TOTAL_MS    300000 // a 16 s passive sweep of all 16 channels fits
#define I154_CHANNEL_OVERHEAD_MS  20     // channel switch and message sending per channel, for the P4's deadline
#define I154_SCAN_ED_MS           10     // the energy detect passive and active run per channel before listening
#define I154_SCAN_GRACE_MS        3000   // on top of the expected time before the P4 gives up on DONE
#define I154_SCAN_ACK_MS          3000   // the C6 answers SCAN_START with ACK (or DONE) within this

typedef struct {
    uint8_t  seq;          // echoed in ED/FRAME/DONE, so a late answer is not taken for the next scan's
    uint8_t  mode;         // i154_mode_t
    uint32_t channel_mask; // within I154_CHANNEL_MASK_ALL, not 0
    uint16_t dwell_ms;
} i154_scan_req_t;

// passive + ED over all channels, a dwell long enough for a chatty Zigbee network
#define I154_SCAN_REQ_DEFAULT {0, I154_MODE_PASSIVE, I154_CHANNEL_MASK_ALL, 2000}

typedef struct {
    uint8_t  modes;        // I154_MODE_BIT() of every mode the C6 runs (active may be left out by a build flag)
    uint32_t channel_mask; // channels it scans
    uint8_t  table_size;   // networks it keeps per scan
} i154_caps_t;

typedef struct {
    uint8_t seq;
    uint8_t channel;   // 11..26: check with i154_ed_valid, the decoder checks lengths only
    int8_t  level_dbm; // the radio's energy detect, uncalibrated
} i154_ed_t;

typedef enum {
    I154_NET_UNKNOWN,
    I154_NET_ZIGBEE,
    I154_NET_THREAD,
} i154_net_t;

#define I154_FRAME_F_BEACON      0x01 // a beacon was heard
#define I154_FRAME_F_PERMIT_JOIN 0x02 // the last beacon allowed joining (Zigbee association permit, Thread joining)
#define I154_FRAME_F_SECURED     0x04 // a frame with the MAC security bit was heard
#define I154_FRAME_F_EXT_PAN     0x08 // ext_pan_id is from a beacon
#define I154_FRAME_F_NAME        0x10 // name is from a Thread beacon
#define I154_NAME_LEN            16

typedef struct {
    uint8_t  seq;
    uint8_t  channel;
    uint16_t pan_id;
    int8_t   rssi_last;
    int8_t   rssi_best;
    uint8_t  lqi_last;
    uint16_t frames;                  // frames counted for this network (saturates)
    uint16_t beacons;                 // beacons among them (saturates)
    uint8_t  guess;                   // i154_net_t, by majority of the per-frame guesses
    uint8_t  flags;                   // I154_FRAME_F_*
    uint8_t  last_type;               // frame type of the last frame
    uint8_t  src_mode;                // address mode of the last source: 0 none, 2 short, 3 extended
    uint64_t src_addr;                // the last source address, as read little-endian from the air
    uint8_t  ext_pan_id[8];           // as on the air (Zigbee little-endian; the Thread order is the beacon's)
    uint8_t  stack_profile;           // Zigbee beacon stack profile (2 = PRO), else 0
    char     name[I154_NAME_LEN + 1]; // Thread network name, NUL-terminated, printable ASCII ('?' for the rest)
} i154_frame_sum_t;

typedef enum {
    I154_DONE_OK,
    I154_DONE_STOPPED,   // SCAN_STOP or a new SCAN_START
    I154_DONE_REFUSED,   // the mode is not in CAPS (e.g. active not built in)
    I154_DONE_BAD_PARAM, // i154_scan_req_valid failed on the C6
    I154_DONE_RADIO_ERROR,
    I154_DONE_BUSY, // the radio is in use (e.g. another scan)
} i154_done_code_t;

typedef struct {
    uint8_t  seq;
    uint8_t  code;        // i154_done_code_t
    uint32_t frames_rx;   // every frame received
    uint16_t frames_bad;  // of which the parser rejected
    uint8_t  networks;    // entries in the table at the end
    uint16_t dropped;     // admissions refused: the table was full of recently heard networks (i154_dedupe_t.dropped)
    uint16_t evicted;     // networks that made way for newer ones (i154_dedupe_t.evicted)
    uint16_t send_failed; // messages the C6 failed to send
} i154_done_t;

typedef struct {
    uint8_t type; // i154_msg_type_t
    union {
        uint8_t          stop_seq; // SCAN_STOP
        uint8_t          ack_seq;  // ACK
        i154_scan_req_t  scan;
        i154_caps_t      caps;
        i154_ed_t        ed;
        i154_frame_sum_t frame;
        i154_done_t      done;
    } u;
} i154_msg_t;

// Bytes written, or 0 if cap is too small or the type is unknown
size_t i154_encode(i154_msg_t const *msg, uint8_t *buf, size_t cap);

typedef enum {
    I154_DEC_OK,
    I154_DEC_BAD_VERSION,
    I154_DEC_BAD_TYPE,
    I154_DEC_BAD_LEN,
} i154_dec_t;

// The length must match the type exactly, except CAPS, which may carry fields of a later version after its frozen
// prefix. A FRAME's name is NUL-terminated and printable after decoding. Values are not range-checked: see
// i154_ed_valid and i154_frame_sum_valid
i154_dec_t i154_decode(uint8_t const *buf, size_t len, i154_msg_t *out);

// A CAPS_REQ of any protocol version, for the C6 to answer with its CAPS
bool i154_is_caps_req(uint8_t const *buf, size_t len);

bool i154_channel_valid(uint8_t channel); // 11..26
bool i154_ed_valid(i154_ed_t const *ed);
// Channel 11..26, a known guess and source address mode
bool i154_frame_sum_valid(i154_frame_sum_t const *f);

// Mode known, channel mask within 11..26 and not empty, dwell within the mode's range, whole scan under the cap
bool i154_scan_req_valid(i154_scan_req_t const *r);

// The C6 runs this request: valid, the mode in caps->modes and every channel in caps->channel_mask
bool i154_caps_allow(i154_caps_t const *caps, i154_scan_req_t const *r);

// How long a valid request should take, and the point after which the P4 stops waiting for DONE (#172 watch list:
// a C6 that stops answering must not hang the scan UI). Elapsed times are uint32_t ms and wrap-safe
uint32_t i154_scan_expected_ms(i154_scan_req_t const *r);
bool     i154_scan_overdue(i154_scan_req_t const *r, uint32_t started_ms, uint32_t now_ms);

// No ACK (or DONE) for the SCAN_START sent at started_ms by now: the C6 lost it, or it stopped answering
bool i154_scan_no_start(uint32_t started_ms, uint32_t now_ms);

// ---- 802.15.4 MAC header parser (IEEE 802.15.4-2006 and -2015 frame versions) ----

#define I154_PSDU_MAX 127 // aMaxPHYPacketSize

typedef enum {
    I154_FT_BEACON = 0,
    I154_FT_DATA   = 1,
    I154_FT_ACK    = 2,
    I154_FT_CMD    = 3,
} i154_frame_type_t;

#define I154_ADDR_NONE  0
#define I154_ADDR_SHORT 2
#define I154_ADDR_EXT   3

#define I154_CMD_BEACON_REQUEST 0x07

typedef enum {
    I154_MAC_OK,
    I154_MAC_SHORT,       // the frame ends inside a header field it announces
    I154_MAC_BAD,         // reserved address mode or frame version, or longer than a PSDU
    I154_MAC_UNSUPPORTED, // a frame type this parser does not read (multipurpose, fragment, extended)
} i154_mac_res_t;

typedef struct {
    uint8_t  frame_type; // i154_frame_type_t
    uint8_t  version;    // 0 = 2003, 1 = 2006, 2 = 2015
    bool     security;
    bool     pan_comp;
    bool     ie_present;
    bool     has_seq;
    uint8_t  seq;
    uint8_t  dst_mode, src_mode; // I154_ADDR_*
    bool     has_dst_pan, has_src_pan;
    uint16_t dst_pan, src_pan; // with PAN ID compression the absent source PAN is the destination PAN
    uint64_t dst_addr, src_addr;
    uint8_t  hdr_len;     // MAC header incl. the auxiliary security header. Header IEs (2015, ie_present) are NOT
                          // skipped: with ie_present the IEs start at hdr_len and the payload offset is unknown
    uint8_t  payload_len; // up to the end of the given frame
    // Hints from the plaintext payload, only when the MAC payload is not secured and carries no IEs
    uint8_t  cmd_id;      // for a command frame, else 0
    uint8_t  guess;       // i154_net_t
    bool     permit_join; // beacon: association permit (Zigbee) or joining permitted (Thread)
    bool     has_ext_pan;
    uint8_t  ext_pan_id[8];
    uint8_t  stack_profile;
    char     name[I154_NAME_LEN + 1];
} i154_mac_t;

// Parses a received MPDU without the PHY length byte and without the 2-byte FCS (the radio checks the FCS). Every
// field is bounds-checked: frames come off the air. On anything but I154_MAC_OK *out is unspecified
i154_mac_res_t i154_mac_parse(uint8_t const *frame, size_t len, i154_mac_t *out);

// The network a frame belongs to: its source PAN (beacons), else its destination PAN; false for frames without a
// PAN ID, and for the broadcast PAN 0xFFFF (beacon requests)
bool i154_mac_pan(i154_mac_t const *m, uint16_t *pan);

// ---- dedupe table: one entry per channel + PAN, so the C6 sends a few summaries instead of every frame ----
// Flood resistance: a PAN is admitted on its second frame within I154_DEDUPE_CONFIRM_MS (a one-off phantom, e.g. a
// corrupt frame that passed the FCS, never is), except a beacon with a recognised Zigbee/Thread payload, which an
// active scan gets only once. A new network evicts the least recently heard entry only when that one was quiet for
// I154_DEDUPE_KEEP_MS, else it is dropped. FRAMEs leave through i154_dedupe_next_due: at most one per network per
// I154_FRAME_MIN_INTERVAL_MS and I154_FRAME_BUDGET per I154_FRAME_WINDOW_MS overall.

#define I154_DEDUPE_SIZE           32
#define I154_DEDUPE_CANDIDATES     8
#define I154_DEDUPE_CONFIRM_MS     60000 // Zigbee routers send link status about every 15 s
#define I154_DEDUPE_KEEP_MS        10000
#define I154_FRAME_MIN_INTERVAL_MS 2000
#define I154_FRAME_WINDOW_MS       1000
#define I154_FRAME_BUDGET          8

typedef struct {
    uint8_t channel;
    int8_t  rssi;
    uint8_t lqi;
} i154_rx_meta_t;

typedef struct {
    bool             used;
    bool             dirty;     // counted since its last FRAME
    bool             ever_sent; // a first FRAME goes before refreshes
    uint32_t         last_ms;   // last heard, for eviction
    uint32_t         sent_ms;
    uint16_t         votes_zigbee, votes_thread;
    i154_frame_sum_t sum; // seq is not kept here: the sender sets it
} i154_dedupe_entry_t;

typedef struct {
    bool     used;
    uint8_t  channel;
    uint16_t pan;
    uint32_t ms;
} i154_dedupe_cand_t;

typedef struct {
    i154_dedupe_entry_t e[I154_DEDUPE_SIZE];
    i154_dedupe_cand_t  cand[I154_DEDUPE_CANDIDATES]; // PANs heard once
    uint16_t            evicted;                      // entries pushed out by newer networks (saturates)
    uint16_t            dropped;                      // admissions refused, table full (saturates)
    uint32_t            window_ms;                    // start of the current FRAME budget window
    uint8_t             window_used;
} i154_dedupe_t;

typedef enum {
    I154_DD_IGNORED, // no PAN ID: nothing to keep
    I154_DD_PENDING, // first frame of an unknown PAN: admitted on the next one
    I154_DD_FULL,    // the table is full of recently heard networks: dropped, counted in dropped
    I154_DD_SEEN,    // counted; the summary changed only in counters and signal
    I154_DD_CHANGED, // the guess, flags, beacon details or source address mode changed
    I154_DD_NEW,     // admitted (an old entry may have made way)
} i154_dd_res_t;

void i154_dedupe_init(i154_dedupe_t *t);

// Counts a parsed frame; *index (may be NULL) is set to its entry for SEEN, CHANGED and NEW, which also mark the
// entry for a FRAME. Counters start at the admitting frame (the candidate's first frame is not counted)
i154_dd_res_t
    i154_dedupe_add(i154_dedupe_t *t, i154_rx_meta_t const *rx, i154_mac_t const *m, uint32_t now_ms, int *index);

int i154_dedupe_count(i154_dedupe_t const *t);

// The entry to send a FRAME for now, or -1: never-sent entries first, then refreshes, within the rate limits.
// flush (at the end of a scan) ignores the limits, so DONE follows the final state of every network. A failed send
// gives the entry back with i154_dedupe_mark
int  i154_dedupe_next_due(i154_dedupe_t *t, uint32_t now_ms, bool flush);
void i154_dedupe_mark(i154_dedupe_t *t, int index);

// ---- the P4's capability probe ----
// A C6 image from before #172 has no handler for I154_MSG_ID_TO_C6 and drops CAPS_REQ silently. The probe sends a
// few CAPS_REQs and settles on TOO_OLD when none is answered, so the scanner can say "update the C6" instead of
// waiting. A CAPS that arrives late still counts. Start it once the hosted link is up, and again each time the
// scanner opens (a C6 may have been updated since).

#define I154_PROBE_TRIES       3
#define I154_PROBE_INTERVAL_MS 1000

typedef enum {
    I154_PROBE_IDLE,       // not started
    I154_PROBE_PENDING,    // waiting for CAPS
    I154_PROBE_OK,         // CAPS arrived: scan within its limits
    I154_PROBE_C6_TOO_OLD, // no answer, or an older protocol version: update the C6
    I154_PROBE_C6_NEWER,   // the C6 speaks a newer protocol: update the P4
} i154_probe_state_t;

typedef struct {
    uint8_t     state; // i154_probe_state_t
    uint8_t     sent;
    uint32_t    last_ms;
    i154_caps_t caps; // valid in I154_PROBE_OK
} i154_probe_t;

void i154_probe_start(i154_probe_t *p);

// True when a CAPS_REQ is due now (the caller sends it); settles on TOO_OLD after the last try went unanswered
bool i154_probe_poll(i154_probe_t *p, uint32_t now_ms);

// Feed every message on I154_MSG_ID_TO_P4. Only a CAPS counts (its frozen prefix, of any version), while PENDING or
// TOO_OLD: our version gives OK, a newer one C6_NEWER, an older one C6_TOO_OLD
void i154_probe_on_msg(i154_probe_t *p, uint8_t const *buf, size_t len);

#ifdef __cplusplus
}
#endif
