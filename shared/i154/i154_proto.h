// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The 802.15.4 scanner (#172): messages between the P4 and the C6's scan service over ESP-Hosted peer data, an
// 802.15.4 MAC header parser, a per-network dedupe table and the P4's "does this C6 scan at all" probe.
// Pure logic, no ESP headers: built into the C6 image, the P4 firmware and the host tests.
//
// Every message starts with [version][type]; the fields follow little-endian in the order of the structs below, and
// every message has one fixed length. A layout change bumps I154_PROTO_VERSION.

#define I154_PROTO_VERSION 1
#define I154_MSG_ID_TO_C6  0x4C03 // next to LORA_MSG_ID_TO_C6 0x4C01 / TO_P4 0x4C02
#define I154_MSG_ID_TO_P4  0x4C04
#define I154_MSG_MAX       64 // largest encoded message (FRAME is 50)

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
#define I154_CHANNEL_OVERHEAD_MS  20     // channel switch + ED, used for the P4's deadline
#define I154_SCAN_GRACE_MS        3000   // on top of the expected time before the P4 gives up on DONE

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
    uint8_t channel;
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
    char     name[I154_NAME_LEN + 1]; // Thread network name, NUL-terminated, "" if none
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
    uint8_t  code;       // i154_done_code_t
    uint32_t frames_rx;  // every frame received
    uint16_t frames_bad; // of which the parser rejected
    uint8_t  networks;   // entries in the table at the end
    uint16_t dropped;    // networks not kept (table full) + messages the C6 failed to send
} i154_done_t;

typedef struct {
    uint8_t type; // i154_msg_type_t
    union {
        uint8_t          stop_seq; // SCAN_STOP
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

// The length must match the type exactly; a FRAME's name is NUL-terminated after decoding
i154_dec_t i154_decode(uint8_t const *buf, size_t len, i154_msg_t *out);

// Mode known, channel mask within 11..26 and not empty, dwell within the mode's range, whole scan under the cap
bool i154_scan_req_valid(i154_scan_req_t const *r);

// The C6 runs this request: valid, the mode in caps->modes and every channel in caps->channel_mask
bool i154_caps_allow(i154_caps_t const *caps, i154_scan_req_t const *r);

// How long a valid request should take, and the point after which the P4 stops waiting for DONE (#172 watch list:
// a C6 that stops answering must not hang the scan UI). Elapsed times are uint32_t ms and wrap-safe
uint32_t i154_scan_expected_ms(i154_scan_req_t const *r);
bool     i154_scan_overdue(i154_scan_req_t const *r, uint32_t started_ms, uint32_t now_ms);

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
    uint8_t  hdr_len;     // MAC header incl. the auxiliary security header; the payload starts here
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

#define I154_DEDUPE_SIZE 32

typedef struct {
    bool             used;
    uint32_t         last_ms; // for evicting the least recently heard entry when the table is full
    uint16_t         votes_zigbee, votes_thread;
    i154_frame_sum_t sum; // seq is not kept here: the sender sets it
} i154_dedupe_entry_t;

typedef struct {
    i154_dedupe_entry_t e[I154_DEDUPE_SIZE];
    uint16_t            evicted; // entries pushed out by newer networks (saturates)
} i154_dedupe_t;

typedef enum {
    I154_DD_IGNORED, // no PAN ID: nothing to keep
    I154_DD_SEEN,    // counted; the summary changed only in counters and signal
    I154_DD_CHANGED, // the guess, flags, beacon details or source address mode changed: worth a FRAME
    I154_DD_NEW,     // a network not in the table yet (an old entry may have made way): worth a FRAME
} i154_dd_res_t;

void i154_dedupe_init(i154_dedupe_t *t);

// Counts a parsed frame; *index (may be NULL) is set to its entry unless IGNORED
i154_dd_res_t i154_dedupe_add(
    i154_dedupe_t *t, uint8_t channel, int8_t rssi, uint8_t lqi, i154_mac_t const *m, uint32_t now_ms, int *index
);

int i154_dedupe_count(i154_dedupe_t const *t);

// ---- the P4's capability probe ----
// A C6 image from before #172 has no handler for I154_MSG_ID_TO_C6 and drops CAPS_REQ silently. The probe sends a
// few CAPS_REQs and settles on TOO_OLD when none is answered, so the scanner can say "update the C6" instead of
// waiting. Start it once the hosted link is up.

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

// Feed every message on I154_MSG_ID_TO_P4 while PENDING; anything but a CAPS or a version mismatch is ignored
void i154_probe_on_msg(i154_probe_t *p, uint8_t const *buf, size_t len);

#ifdef __cplusplus
}
#endif
