// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
// Host tests for the 802.15.4 scanner protocol (#172): messages, MAC header parser, dedupe table, capability probe.
// Frames come off the air, so every fixture is also parsed truncated and from an exactly sized heap copy: ASan
// catches a read past the end.
#include "i154_proto.h"

#include <stdio.h>
#include <stdlib.h>

#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// ---- fixtures: MPDUs without PHR and FCS, laid out as the radio hands them over ----

// Zigbee PRO coordinator beacon: frame v2003, source short 0x0000 on PAN 0x1A62, association permit + PAN coordinator,
// beacon payload protocol 0, stack profile 2, NWK version 2, router and end device capacity, ext PAN 11..88
static uint8_t const zb_beacon[] = {
    0x00, 0x80, 0x8A, 0x62, 0x1A, 0x00, 0x00, // FCF, seq, src PAN, src short
    0xFF, 0xCF, 0x00, 0x00,                   // superframe, GTS, pending addresses
    0x00, 0x22, 0x84, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0xFF, 0xFF, 0xFF, 0x00,
};

// Zigbee NWK link status: MAC data frame, PAN compression, short 0x0000 -> 0xFFFF on PAN 0x1A62; NWK command, v2,
// NWK security on (encrypted after the NWK header)
static uint8_t const zb_data[] = {
    0x41, 0x88, 0x5C, 0x62, 0x1A, 0xFF, 0xFF, 0x00, 0x00,       // MAC header
    0x09, 0x12, 0xFC, 0xFF, 0x00, 0x00, 0x01, 0xB2,             // NWK header
    0x28, 0x01, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, // aux NWK security, ciphertext
};

// Thread MLE advertisement: MAC v2006 data frame without MAC security, PAN 0xFACE, broadcast from an extended source,
// then 6LoWPAN IPHC (0x7F...) carrying UDP to the MLE port
static uint8_t const th_mle[] = {
    0x41, 0xD8, 0x77, 0xCE, 0xFA, 0xFF, 0xFF,       // FCF, seq, dst PAN, dst short
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, // src extended
    0x7F, 0x33, 0xF0, 0x4D, 0x4C, 0x4D, 0x4C, 0x00, 0x15, 0x00, 0x00, 0x00, 0x01, 0x02,
};

// Thread data: MAC v2006, security level 5 with key id mode 1, ack request, PAN compression, short 0x0401 -> 0x0400
static uint8_t const th_secured[] = {
    0x69, 0x98, 0x10, 0xCE, 0xFA, 0x00, 0x04, 0x01, 0x04, // MAC header
    0x0D, 0x44, 0x33, 0x22, 0x11, 0x01,                   // aux security: control, frame counter, key index
    0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01, 0x02, 0x03, 0x04, // ciphertext + MIC
};

// Thread (pre-1.2) beacon: v2006, extended source on PAN 0xFACE, no association permit; protocol 3, version 2,
// joining permitted, name "MyHome", ext PAN DE AD 00 BE EF 00 CA FE
static uint8_t const th_beacon[] = {
    0x00, 0xD0, 0x21, 0xCE, 0xFA, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, // FCF, seq, src PAN, src ext
    0xFF, 0x4F, 0x00, 0x00,                                                       // superframe, GTS, pending
    0x03, 0x21, 'M',  'y',  'H',  'o',  'm',  'e',  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0xDE, 0xAD, 0x00, 0xBE, 0xEF, 0x00, 0xCA, 0xFE,
};

// MAC beacon request (what an active scan sends): command 0x07 to broadcast short on the broadcast PAN
static uint8_t const beacon_req[] = {0x03, 0x08, 0x42, 0xFF, 0xFF, 0xFF, 0xFF, 0x07};

static uint8_t const ack[] = {0x02, 0x00, 0x42};

// Parses from a heap copy of exactly n bytes, so ASan sees any read past the frame
static i154_mac_res_t parse_n(uint8_t const *frame, size_t n, i154_mac_t *m) {
    uint8_t *copy = malloc(n ? n : 1);
    if (!copy) {
        printf("FAIL out of memory\n");
        exit(1);
    }
    memcpy(copy, frame, n);
    i154_mac_res_t res = i154_mac_parse(copy, n, m);
    free(copy);
    return res;
}

static void test_zigbee_beacon(void) {
    i154_mac_t m;
    uint16_t   pan = 0;
    CHECK(parse_n(zb_beacon, sizeof(zb_beacon), &m) == I154_MAC_OK);
    CHECK(m.frame_type == I154_FT_BEACON && m.version == 0 && !m.security && m.seq == 0x8A);
    CHECK(m.src_mode == I154_ADDR_SHORT && m.src_addr == 0 && m.has_src_pan && m.src_pan == 0x1A62);
    CHECK(m.dst_mode == I154_ADDR_NONE && !m.has_dst_pan);
    CHECK(m.hdr_len == 7 && m.payload_len == sizeof(zb_beacon) - 7);
    CHECK(m.guess == I154_NET_ZIGBEE && m.permit_join && m.stack_profile == 2);
    CHECK(m.has_ext_pan && memcmp(m.ext_pan_id, zb_beacon + 14, 8) == 0);
    CHECK(i154_mac_pan(&m, &pan) && pan == 0x1A62);
}

static void test_zigbee_beacon_cut(void) {
    i154_mac_t m;
    // Cut anywhere: inside the MAC header or the superframe/GTS/pending fields is SHORT; after them the beacon still
    // parses, but without a complete Zigbee payload there is no guess
    for (size_t n = 0; n < sizeof(zb_beacon); n++) {
        i154_mac_res_t res = parse_n(zb_beacon, n, &m);
        if (n < 11) {
            CHECK(res == I154_MAC_SHORT);
        } else {
            CHECK(res == I154_MAC_OK && m.guess == I154_NET_UNKNOWN && !m.has_ext_pan);
        }
    }
}

static void test_zigbee_data(void) {
    i154_mac_t m;
    uint16_t   pan = 0;
    CHECK(parse_n(zb_data, sizeof(zb_data), &m) == I154_MAC_OK);
    CHECK(m.frame_type == I154_FT_DATA && m.pan_comp && m.version == 0);
    CHECK(m.has_dst_pan && m.dst_pan == 0x1A62 && m.dst_addr == 0xFFFF);
    CHECK(m.has_src_pan && m.src_pan == 0x1A62 && m.src_addr == 0x0000); // compressed: the destination's PAN
    CHECK(m.hdr_len == 9 && m.guess == I154_NET_ZIGBEE);
    CHECK(i154_mac_pan(&m, &pan) && pan == 0x1A62);

    // A NWK header cut short is no evidence
    CHECK(parse_n(zb_data, 9 + 7, &m) == I154_MAC_OK && m.guess == I154_NET_UNKNOWN);
    for (size_t n = 0; n < 9; n++) {
        CHECK(parse_n(zb_data, n, &m) == I154_MAC_SHORT);
    }
}

static void test_thread_mle(void) {
    i154_mac_t m;
    CHECK(parse_n(th_mle, sizeof(th_mle), &m) == I154_MAC_OK);
    CHECK(m.frame_type == I154_FT_DATA && m.version == 1 && !m.security);
    CHECK(m.dst_pan == 0xFACE && m.dst_addr == 0xFFFF && m.src_mode == I154_ADDR_EXT);
    CHECK(m.src_addr == 0x0807060504030201ull); // little-endian on the air
    CHECK(m.hdr_len == 15 && m.guess == I154_NET_THREAD);
    for (size_t n = 0; n < 15; n++) {
        CHECK(parse_n(th_mle, n, &m) == I154_MAC_SHORT);
    }
}

static void test_thread_secured(void) {
    i154_mac_t m;
    CHECK(parse_n(th_secured, sizeof(th_secured), &m) == I154_MAC_OK);
    CHECK(m.security && m.version == 1 && m.dst_addr == 0x0400 && m.src_addr == 0x0401);
    CHECK(m.hdr_len == 9 + 6 && m.payload_len == sizeof(th_secured) - 15);
    CHECK(m.guess == I154_NET_THREAD);
    // The aux security header is part of the header: cut inside it is SHORT
    for (size_t n = 9; n < 15; n++) {
        CHECK(parse_n(th_secured, n, &m) == I154_MAC_SHORT);
    }
    CHECK(parse_n(th_secured, 15, &m) == I154_MAC_OK && m.payload_len == 0);

    // Key id mode 3 needs 9 key identifier bytes after the frame counter
    uint8_t f[sizeof(th_secured)];
    memcpy(f, th_secured, sizeof(f));
    f[9] = 0x1D;
    CHECK(parse_n(f, 9 + 1 + 4 + 8, &m) == I154_MAC_SHORT);
    CHECK(parse_n(f, 9 + 1 + 4 + 9, &m) == I154_MAC_OK && m.hdr_len == 23);
}

static void test_thread_beacon(void) {
    i154_mac_t m;
    uint16_t   pan = 0;
    CHECK(parse_n(th_beacon, sizeof(th_beacon), &m) == I154_MAC_OK);
    CHECK(m.frame_type == I154_FT_BEACON && m.guess == I154_NET_THREAD);
    CHECK(m.permit_join && strcmp(m.name, "MyHome") == 0);
    CHECK(m.has_ext_pan && memcmp(m.ext_pan_id, "\xDE\xAD\x00\xBE\xEF\x00\xCA\xFE", 8) == 0);
    CHECK(i154_mac_pan(&m, &pan) && pan == 0xFACE);

    // A name of 16 characters fills the field without a NUL; control bytes are not passed to the UI
    uint8_t f[sizeof(th_beacon)];
    memcpy(f, th_beacon, sizeof(f));
    memcpy(f + 19, "ABCDEFGHIJKLMNO\x07", 16);
    CHECK(parse_n(f, sizeof(f), &m) == I154_MAC_OK && strcmp(m.name, "ABCDEFGHIJKLMNO?") == 0);
}

static void test_commands_and_acks(void) {
    i154_mac_t m;
    uint16_t   pan = 0;
    CHECK(parse_n(beacon_req, sizeof(beacon_req), &m) == I154_MAC_OK);
    CHECK(m.frame_type == I154_FT_CMD && m.cmd_id == I154_CMD_BEACON_REQUEST && !i154_mac_pan(&m, &pan));
    CHECK(parse_n(beacon_req, sizeof(beacon_req) - 1, &m) == I154_MAC_SHORT); // a command needs its id

    CHECK(parse_n(ack, sizeof(ack), &m) == I154_MAC_OK);
    CHECK(m.frame_type == I154_FT_ACK && m.seq == 0x42 && m.hdr_len == 3 && !i154_mac_pan(&m, &pan));
}

static void test_rejects(void) {
    i154_mac_t m;
    uint8_t    f[I154_PSDU_MAX + 1] = {0};

    f[0] = 0x41, f[1] = 0x84; // destination address mode 1 is reserved
    CHECK(parse_n(f, 9, &m) == I154_MAC_BAD);
    f[1] = 0x48; // source address mode 1
    CHECK(parse_n(f, 9, &m) == I154_MAC_BAD);
    f[1] = 0xB8; // frame version 3
    CHECK(parse_n(f, 9, &m) == I154_MAC_BAD);
    f[0] = 0x05, f[1] = 0x00; // multipurpose frame
    CHECK(parse_n(f, 9, &m) == I154_MAC_UNSUPPORTED);
    f[0] = 0x02;
    CHECK(parse_n(f, sizeof(f), &m) == I154_MAC_BAD); // longer than a PSDU
    CHECK(parse_n(f, I154_PSDU_MAX, &m) == I154_MAC_OK);
    CHECK(parse_n(f, 1, &m) == I154_MAC_SHORT);
}

// 802.15.4-2015 frame version 2: the PAN ID presence table, sequence number suppression, IEs
static void test_frame_version_2015(void) {
    i154_mac_t m;
    // Data, both extended, PAN compression 0: only the destination PAN is present
    uint8_t    ext_ext[] = {0x01, 0xEC, 0x01, 0xCE, 0xFA, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 0x7F};
    CHECK(parse_n(ext_ext, sizeof(ext_ext), &m) == I154_MAC_OK);
    CHECK(m.version == 2 && m.has_dst_pan && m.dst_pan == 0xFACE && m.hdr_len == 21);
    CHECK(m.has_src_pan && m.src_pan == 0xFACE);

    // Short to short, compression 0: both PANs
    uint8_t short_short[] = {0x01, 0xA8, 0x02, 0x34, 0x12, 0x01, 0x00, 0x78, 0x56, 0x02, 0x00};
    CHECK(parse_n(short_short, sizeof(short_short), &m) == I154_MAC_OK);
    CHECK(m.dst_pan == 0x1234 && m.src_pan == 0x5678 && m.hdr_len == 11);

    // No addresses, compression 1: a destination PAN alone. Sequence number suppressed, IEs present: no hints
    uint8_t pan_only[] = {0x41, 0x23, 0xCE, 0xFA, 0x00, 0x3F};
    CHECK(parse_n(pan_only, sizeof(pan_only), &m) == I154_MAC_OK);
    CHECK(!m.has_seq && m.ie_present && m.has_dst_pan && m.dst_pan == 0xFACE && m.hdr_len == 4);
    CHECK(m.guess == I154_NET_THREAD);
    CHECK(parse_n(pan_only, 3, &m) == I154_MAC_SHORT);
}

// Random bytes of every length must parse or be rejected without reading past the end; a parsed header lies inside
static void test_garbage(void) {
    uint32_t   x                = 0x12345678;
    uint8_t    f[I154_PSDU_MAX] = {0};
    i154_mac_t m;
    for (int round = 0; round < 20000; round++) {
        size_t n = (size_t)(round % (I154_PSDU_MAX + 1));
        for (size_t i = 0; i < n; i++) {
            x    = x * 1664525u + 1013904223u;
            f[i] = (uint8_t)(x >> 24);
        }
        if (parse_n(f, n, &m) == I154_MAC_OK) {
            CHECK(m.hdr_len + m.payload_len == n);
            CHECK(strlen(m.name) <= I154_NAME_LEN);
        }
    }
}

// ---- messages ----

static i154_msg_t round_trip(i154_msg_t const *in, size_t expect_len) {
    uint8_t    buf[I154_MSG_MAX];
    i154_msg_t out;
    memset(&out, 0xA5, sizeof(out));
    size_t n = i154_encode(in, buf, sizeof(buf));
    CHECK(n == expect_len);
    CHECK(i154_decode(buf, n, &out) == I154_DEC_OK && out.type == in->type);
    // One byte short or long is a bad length, never a misread
    CHECK(i154_decode(buf, n - 1, &out) == I154_DEC_BAD_LEN);
    CHECK(i154_decode(buf, n + 1, &out) == I154_DEC_BAD_LEN);
    CHECK(i154_encode(in, buf, n - 1) == 0);
    i154_decode(buf, n, &out);
    return out;
}

static void test_messages_small(void) {
    i154_msg_t m = {.type = I154_MSG_CAPS_REQ}, got;
    round_trip(&m, 2);

    m.type   = I154_MSG_SCAN_START;
    m.u.scan = (i154_scan_req_t){7, I154_MODE_ACTIVE, 0x02000800u, 1234};
    got      = round_trip(&m, 10);
    CHECK(got.u.scan.seq == 7 && got.u.scan.mode == I154_MODE_ACTIVE);
    CHECK(got.u.scan.channel_mask == 0x02000800u && got.u.scan.dwell_ms == 1234);

    m = (i154_msg_t){.type = I154_MSG_SCAN_STOP, .u.stop_seq = 9};
    CHECK(round_trip(&m, 3).u.stop_seq == 9);

    m   = (i154_msg_t){.type = I154_MSG_CAPS, .u.caps = {0x0E, I154_CHANNEL_MASK_ALL, 32}};
    got = round_trip(&m, 8);
    CHECK(got.u.caps.modes == 0x0E && got.u.caps.channel_mask == I154_CHANNEL_MASK_ALL && got.u.caps.table_size == 32);

    m   = (i154_msg_t){.type = I154_MSG_ED, .u.ed = {3, 26, -87}};
    got = round_trip(&m, 5);
    CHECK(got.u.ed.seq == 3 && got.u.ed.channel == 26 && got.u.ed.level_dbm == -87);

    m   = (i154_msg_t){.type = I154_MSG_DONE, .u.done = {4, I154_DONE_STOPPED, 0x01020304u, 513, 31, 65535}};
    got = round_trip(&m, 13);
    CHECK(got.u.done.seq == 4 && got.u.done.code == I154_DONE_STOPPED && got.u.done.frames_rx == 0x01020304u);
    CHECK(got.u.done.frames_bad == 513 && got.u.done.networks == 31 && got.u.done.dropped == 65535);
}

static void test_message_frame(void) {
    i154_msg_t m        = {.type = I154_MSG_FRAME};
    m.u.frame.seq       = 5;
    m.u.frame.channel   = 15;
    m.u.frame.pan_id    = 0x1A62;
    m.u.frame.rssi_last = -71, m.u.frame.rssi_best = -40, m.u.frame.lqi_last = 200;
    m.u.frame.frames = 1000, m.u.frame.beacons = 3, m.u.frame.guess = I154_NET_THREAD;
    m.u.frame.flags = I154_FRAME_F_BEACON | I154_FRAME_F_NAME, m.u.frame.last_type = I154_FT_DATA;
    m.u.frame.src_mode = I154_ADDR_EXT, m.u.frame.src_addr = 0x0123456789ABCDEFull;
    memcpy(m.u.frame.ext_pan_id, "\xDE\xAD\x00\xBE\xEF\x00\xCA\xFE", 8);
    m.u.frame.stack_profile = 2;
    strcpy(m.u.frame.name, "ABCDEFGHIJKLMNOP"); // the full 16 characters

    i154_msg_t got = round_trip(&m, 50);
    CHECK(got.u.frame.seq == 5 && got.u.frame.channel == 15 && got.u.frame.pan_id == 0x1A62);
    CHECK(got.u.frame.rssi_last == -71 && got.u.frame.rssi_best == -40 && got.u.frame.lqi_last == 200);
    CHECK(got.u.frame.frames == 1000 && got.u.frame.beacons == 3 && got.u.frame.guess == I154_NET_THREAD);
    CHECK(got.u.frame.flags == (I154_FRAME_F_BEACON | I154_FRAME_F_NAME) && got.u.frame.last_type == I154_FT_DATA);
    CHECK(got.u.frame.src_mode == I154_ADDR_EXT && got.u.frame.src_addr == 0x0123456789ABCDEFull);
    CHECK(memcmp(got.u.frame.ext_pan_id, m.u.frame.ext_pan_id, 8) == 0 && got.u.frame.stack_profile == 2);
    CHECK(strcmp(got.u.frame.name, "ABCDEFGHIJKLMNOP") == 0);

    strcpy(m.u.frame.name, "Hue");
    CHECK(strcmp(round_trip(&m, 50).u.frame.name, "Hue") == 0);
}

static void test_message_rejects(void) {
    uint8_t    buf[I154_MSG_MAX];
    i154_msg_t m = {.type = I154_MSG_CAPS_REQ}, out;
    size_t     n = i154_encode(&m, buf, sizeof(buf));
    CHECK(i154_decode(buf, 0, &out) == I154_DEC_BAD_LEN);
    CHECK(i154_decode(buf, 1, &out) == I154_DEC_BAD_LEN);
    buf[0] = I154_PROTO_VERSION + 1;
    CHECK(i154_decode(buf, n, &out) == I154_DEC_BAD_VERSION);
    buf[0] = I154_PROTO_VERSION, buf[1] = 0x7E;
    CHECK(i154_decode(buf, n, &out) == I154_DEC_BAD_TYPE);
    m.type = 0x7E;
    CHECK(i154_encode(&m, buf, sizeof(buf)) == 0);
}

// ---- scan requests ----

static void test_scan_req(void) {
    i154_scan_req_t r = I154_SCAN_REQ_DEFAULT;
    CHECK(r.mode == I154_MODE_PASSIVE && i154_scan_req_valid(&r)); // the default never transmits

    i154_scan_req_t bad = r;
    bad.channel_mask    = 0;
    CHECK(!i154_scan_req_valid(&bad));
    bad.channel_mask = 1u << 10;
    CHECK(!i154_scan_req_valid(&bad));
    bad.channel_mask = 1u << 27;
    CHECK(!i154_scan_req_valid(&bad));
    bad = r, bad.mode = 0;
    CHECK(!i154_scan_req_valid(&bad));
    bad.mode = I154_MODE_ACTIVE + 1;
    CHECK(!i154_scan_req_valid(&bad));

    i154_scan_req_t ed = {0, I154_MODE_ED, I154_CHANNEL_MASK_ALL, I154_ED_DWELL_MAX_MS};
    CHECK(i154_scan_req_valid(&ed));
    ed.dwell_ms = I154_ED_DWELL_MAX_MS + 1;
    CHECK(!i154_scan_req_valid(&ed));
    ed.dwell_ms = 0;
    CHECK(!i154_scan_req_valid(&ed));

    i154_scan_req_t act = {0, I154_MODE_ACTIVE, I154_CHANNEL_MASK_ALL, I154_ACTIVE_DWELL_MAX_MS + 1};
    CHECK(!i154_scan_req_valid(&act));
    act.dwell_ms = I154_LISTEN_DWELL_MIN_MS - 1;
    CHECK(!i154_scan_req_valid(&act));

    // The whole scan is capped: 10 channels at 30 s fit, 11 do not
    i154_scan_req_t slow = {0, I154_MODE_PASSIVE, 0x3FFu << 11, I154_PASSIVE_DWELL_MAX_MS};
    CHECK(i154_scan_req_valid(&slow));
    slow.channel_mask = 0x7FFu << 11;
    CHECK(!i154_scan_req_valid(&slow));
}

static void test_caps_and_deadline(void) {
    i154_caps_t     caps = {I154_MODE_BIT(I154_MODE_ED) | I154_MODE_BIT(I154_MODE_PASSIVE), I154_CHANNEL_MASK_ALL, 32};
    i154_scan_req_t r    = I154_SCAN_REQ_DEFAULT;
    CHECK(i154_caps_allow(&caps, &r));
    r.mode = I154_MODE_ACTIVE; // built without active
    CHECK(!i154_caps_allow(&caps, &r));
    caps.modes        |= I154_MODE_BIT(I154_MODE_ACTIVE);
    caps.channel_mask  = 1u << 15;
    CHECK(!i154_caps_allow(&caps, &r));
    r.channel_mask = 1u << 15;
    CHECK(i154_caps_allow(&caps, &r));

    i154_scan_req_t p = I154_SCAN_REQ_DEFAULT;
    uint32_t        e = i154_scan_expected_ms(&p);
    CHECK(e == 16u * (2000 + I154_CHANNEL_OVERHEAD_MS));
    CHECK(!i154_scan_overdue(&p, 1000, 1000 + e + I154_SCAN_GRACE_MS));
    CHECK(i154_scan_overdue(&p, 1000, 1000 + e + I154_SCAN_GRACE_MS + 1));
    uint32_t start = 0xFFFFF000u; // the ms clock wraps during the scan
    CHECK(!i154_scan_overdue(&p, start, start + e));
    CHECK(i154_scan_overdue(&p, start, start + e + I154_SCAN_GRACE_MS + 1));
}

// ---- dedupe ----

static i154_mac_t mac_of(uint8_t const *frame, size_t n) {
    i154_mac_t m;
    CHECK(i154_mac_parse(frame, n, &m) == I154_MAC_OK);
    return m;
}

static void test_dedupe_basic(void) {
    i154_dedupe_t t;
    int           idx = -1;
    i154_dedupe_init(&t);

    i154_mac_t data = mac_of(zb_data, sizeof(zb_data)), beacon = mac_of(zb_beacon, sizeof(zb_beacon));
    i154_mac_t req = mac_of(beacon_req, sizeof(beacon_req));
    CHECK(i154_dedupe_add(&t, 15, -60, 180, &req, 0, &idx) == I154_DD_IGNORED);
    CHECK(i154_dedupe_add(&t, 15, -60, 180, &data, 10, &idx) == I154_DD_NEW && i154_dedupe_count(&t) == 1);
    CHECK(i154_dedupe_add(&t, 15, -50, 190, &data, 20, &idx) == I154_DD_SEEN);
    CHECK(i154_dedupe_add(&t, 15, -70, 100, &beacon, 30, &idx) == I154_DD_CHANGED); // beacon details arrived
    CHECK(i154_dedupe_add(&t, 15, -70, 100, &beacon, 40, &idx) == I154_DD_SEEN);

    i154_frame_sum_t const *s = &t.e[idx].sum;
    CHECK(s->channel == 15 && s->pan_id == 0x1A62 && s->frames == 4 && s->beacons == 2);
    CHECK(s->rssi_last == -70 && s->rssi_best == -50 && s->lqi_last == 100 && s->guess == I154_NET_ZIGBEE);
    CHECK(s->flags == (I154_FRAME_F_BEACON | I154_FRAME_F_PERMIT_JOIN | I154_FRAME_F_EXT_PAN));
    CHECK(s->stack_profile == 2 && memcmp(s->ext_pan_id, zb_beacon + 14, 8) == 0);

    // The same PAN on another channel is another network
    CHECK(i154_dedupe_add(&t, 20, -60, 180, &data, 50, NULL) == I154_DD_NEW && i154_dedupe_count(&t) == 2);
}

static void test_dedupe_votes(void) {
    i154_dedupe_t t;
    int           idx;
    i154_dedupe_init(&t);
    i154_mac_t zb = mac_of(zb_data, sizeof(zb_data)), th = mac_of(th_secured, sizeof(th_secured));
    th.dst_pan = th.src_pan = 0x1A62; // the same PAN heard with conflicting frames

    i154_dedupe_add(&t, 11, -60, 0, &zb, 0, &idx);
    CHECK(t.e[idx].sum.guess == I154_NET_ZIGBEE);
    CHECK(i154_dedupe_add(&t, 11, -60, 0, &th, 1, &idx) == I154_DD_CHANGED); // secured flag
    CHECK(t.e[idx].sum.guess == I154_NET_ZIGBEE);                            // a tie keeps the guess
    CHECK(i154_dedupe_add(&t, 11, -60, 0, &th, 2, &idx) == I154_DD_CHANGED); // majority flips it
    CHECK(t.e[idx].sum.guess == I154_NET_THREAD);
}

static void test_dedupe_full(void) {
    i154_dedupe_t t;
    i154_dedupe_init(&t);
    i154_mac_t m = mac_of(zb_data, sizeof(zb_data));
    for (int i = 0; i < I154_DEDUPE_SIZE; i++) {
        m.dst_pan = m.src_pan = (uint16_t)(0x100 + i);
        CHECK(i154_dedupe_add(&t, 11, -60, 0, &m, (uint32_t)(1000 + i), NULL) == I154_DD_NEW);
    }
    // Refresh the first one, so the second is now the least recently heard
    m.dst_pan = m.src_pan = 0x100;
    CHECK(i154_dedupe_add(&t, 11, -60, 0, &m, 5000, NULL) == I154_DD_SEEN);

    int idx   = -1;
    m.dst_pan = m.src_pan = 0x999;
    CHECK(i154_dedupe_add(&t, 11, -60, 0, &m, 6000, &idx) == I154_DD_NEW);
    CHECK(i154_dedupe_count(&t) == I154_DEDUPE_SIZE && t.evicted == 1 && idx == 1);
    CHECK(t.e[idx].sum.pan_id == 0x999 && t.e[idx].sum.frames == 1);

    m.dst_pan = m.src_pan = 0x101; // the evicted network comes back as new
    CHECK(i154_dedupe_add(&t, 11, -60, 0, &m, 7000, NULL) == I154_DD_NEW && t.evicted == 2);
}

// ---- capability probe: an old C6 never answers, the scanner must not wait forever ----

static void test_probe_old_c6(void) {
    i154_probe_t p;
    i154_probe_start(&p);
    CHECK(p.state == I154_PROBE_PENDING);
    CHECK(i154_probe_poll(&p, 100));
    CHECK(!i154_probe_poll(&p, 100 + I154_PROBE_INTERVAL_MS - 1));
    CHECK(i154_probe_poll(&p, 100 + I154_PROBE_INTERVAL_MS));
    CHECK(i154_probe_poll(&p, 100 + 2 * I154_PROBE_INTERVAL_MS));
    CHECK(p.state == I154_PROBE_PENDING);
    CHECK(!i154_probe_poll(&p, 100 + 3 * I154_PROBE_INTERVAL_MS));
    CHECK(p.state == I154_PROBE_C6_TOO_OLD);
    CHECK(!i154_probe_poll(&p, 100 + 10 * I154_PROBE_INTERVAL_MS)); // settled: no more requests

    i154_probe_t idle = {0};
    CHECK(!i154_probe_poll(&idle, 0) && idle.state == I154_PROBE_IDLE);
}

static void test_probe_answers(void) {
    uint8_t      buf[I154_MSG_MAX];
    i154_probe_t p;
    i154_msg_t   caps = {.type = I154_MSG_CAPS, .u.caps = {0x06, I154_CHANNEL_MASK_ALL, 32}};
    i154_msg_t   ed   = {.type = I154_MSG_ED, .u.ed = {1, 11, -90}};

    i154_probe_start(&p);
    i154_probe_poll(&p, 0);
    size_t n = i154_encode(&ed, buf, sizeof(buf));
    i154_probe_on_msg(&p, buf, n); // not an answer
    i154_probe_on_msg(&p, buf, 1); // too short to tell
    CHECK(p.state == I154_PROBE_PENDING);
    n = i154_encode(&caps, buf, sizeof(buf));
    i154_probe_on_msg(&p, buf, n);
    CHECK(p.state == I154_PROBE_OK && p.caps.modes == 0x06 && p.caps.table_size == 32);
    CHECK(!i154_probe_poll(&p, 5000));

    i154_probe_start(&p);
    buf[0] = I154_PROTO_VERSION - 1;
    i154_probe_on_msg(&p, buf, n);
    CHECK(p.state == I154_PROBE_C6_TOO_OLD);
    i154_probe_start(&p);
    buf[0] = I154_PROTO_VERSION + 1;
    i154_probe_on_msg(&p, buf, n);
    CHECK(p.state == I154_PROBE_C6_NEWER);
}

int main(void) {
    test_zigbee_beacon();
    test_zigbee_beacon_cut();
    test_zigbee_data();
    test_thread_mle();
    test_thread_secured();
    test_thread_beacon();
    test_commands_and_acks();
    test_rejects();
    test_frame_version_2015();
    test_garbage();
    test_messages_small();
    test_message_frame();
    test_message_rejects();
    test_scan_req();
    test_caps_and_deadline();
    test_dedupe_basic();
    test_dedupe_votes();
    test_dedupe_full();
    test_probe_old_c6();
    test_probe_answers();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("i154_proto_test: all passed\n");
    return 0;
}
