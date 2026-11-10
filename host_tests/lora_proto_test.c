// Host tests for the P4 <-> C6 LoRa message protocol
#include "lora_proto.h"

#include <stdio.h>

#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static lora_msg_t round_trip(lora_msg_t const *in, size_t *encoded_len) {
    uint8_t    buf[LORA_MSG_MAX];
    lora_msg_t out;
    memset(&out, 0xA5, sizeof(out));
    *encoded_len = lora_encode(in, buf, sizeof(buf));
    CHECK(*encoded_len > 0);
    CHECK(lora_decode(buf, *encoded_len, &out) == LORA_DEC_OK);
    CHECK(out.type == in->type);
    return out;
}

static void round_trip_each_type(void) {
    size_t n;

    lora_msg_t cfg = {.type = LORA_MSG_CONFIG, .u.config = LORA_CONFIG_DEFAULT};
    lora_msg_t got = round_trip(&cfg, &n);
    CHECK(got.u.config.freq_hz == 869618000);
    CHECK(got.u.config.bw_hz == 62500);
    CHECK(got.u.config.sf == 7);
    CHECK(got.u.config.cr == 5);
    CHECK(got.u.config.sync_word == 0x12);
    CHECK(got.u.config.preamble == 16);
    CHECK(got.u.config.tx_power_dbm == 22);

    // A packet to send: its length and bytes
    lora_msg_t send = {.type = LORA_MSG_SEND};
    send.u.tx.len   = 3;
    memcpy(send.u.tx.data, "\x11\x22\x33", 3);
    got = round_trip(&send, &n);
    CHECK(n == 2 + 1 + 3 && got.u.tx.len == 3 && memcmp(got.u.tx.data, "\x11\x22\x33", 3) == 0);

    int lens[] = {255, 1};
    for (int i = 0; i < 2; i++) {
        lora_msg_t rx    = {.type = LORA_MSG_RX_PACKET};
        rx.u.rx.rssi_x10 = -523;
        rx.u.rx.snr_x10  = -75;
        rx.u.rx.c6_ms    = 0xDEADBEEF;
        rx.u.rx.len      = (uint8_t)lens[i];
        for (int j = 0; j < lens[i]; j++) {
            rx.u.rx.data[j] = (uint8_t)(j * 7 + 3);
        }
        got = round_trip(&rx, &n);
        CHECK(n == 11u + (size_t)lens[i]);
        CHECK(got.u.rx.rssi_x10 == -523);
        CHECK(got.u.rx.snr_x10 == -75);
        CHECK(got.u.rx.c6_ms == 0xDEADBEEF);
        CHECK(got.u.rx.len == lens[i]);
        CHECK(memcmp(got.u.rx.data, rx.u.rx.data, (size_t)lens[i]) == 0);
    }

    lora_msg_t st = {
        .type     = LORA_MSG_STATUS,
        .u.status = {
            .state          = LORA_STATE_RX,
            .rssi_x10       = -1052,
            .last_radio_err = -707,
            .packets        = 1,
            .crc_errors     = 2,
            .header_errors  = 3,
            .dropped        = 4,
            .ignored        = 0x01020304,
            .uptime_s       = 754,
            .heap_free      = 123456,
            .heap_min       = 98765,
            .stack_free     = 1500,
        }
    };
    got = round_trip(&st, &n);
    CHECK(got.u.status.state == LORA_STATE_RX);
    CHECK(got.u.status.rssi_x10 == -1052);
    CHECK(got.u.status.last_radio_err == -707);
    CHECK(got.u.status.packets == 1);
    CHECK(got.u.status.crc_errors == 2);
    CHECK(got.u.status.header_errors == 3);
    CHECK(got.u.status.dropped == 4);
    CHECK(got.u.status.ignored == 0x01020304);
    CHECK(got.u.status.uptime_s == 754);
    CHECK(got.u.status.heap_free == 123456);
    CHECK(got.u.status.heap_min == 98765);
    CHECK(got.u.status.stack_free == 1500);

    lora_msg_t res = {.type = LORA_MSG_RESULT, .u.result = {LORA_MSG_SEND, LORA_RESULT_REFUSED, 0}};
    got            = round_trip(&res, &n);
    CHECK(got.u.result.for_type == LORA_MSG_SEND);
    CHECK(got.u.result.code == LORA_RESULT_REFUSED);
    CHECK(got.u.result.radio_err == 0);

    uint8_t bodyless[] = {LORA_MSG_RX_START, LORA_MSG_RX_STOP, LORA_MSG_STATUS_REQ};
    for (size_t i = 0; i < sizeof(bodyless); i++) {
        lora_msg_t m = {.type = bodyless[i]};
        round_trip(&m, &n);
        CHECK(n == 2);
    }
}

static void encode_layout_pinned(void) {
    lora_msg_t    cfg      = {.type = LORA_MSG_CONFIG, .u.config = LORA_CONFIG_DEFAULT};
    uint8_t const expect[] = {0x04, 0x01, 0x50, 0x51, 0xD5, 0x33, 0x24, 0xF4, 0x00,
                              0x00, 0x07, 0x05, 0x12, 0x10, 0x00, 0x16};
    uint8_t       buf[LORA_MSG_MAX];
    CHECK(lora_encode(&cfg, buf, sizeof(buf)) == sizeof(expect));
    CHECK(memcmp(buf, expect, sizeof(expect)) == 0);
}

static void encode_short_buffer(void) {
    lora_msg_t rx = {.type = LORA_MSG_RX_PACKET};
    rx.u.rx.len   = 200;
    uint8_t buf[100];
    CHECK(lora_encode(&rx, buf, sizeof(buf)) == 0);

    lora_msg_t empty_send = {.type = LORA_MSG_SEND}; // nothing to send
    uint8_t    room[LORA_MSG_MAX];
    CHECK(lora_encode(&empty_send, room, sizeof(room)) == 0);

    lora_msg_t unknown = {.type = 0x55};
    uint8_t    big[LORA_MSG_MAX];
    CHECK(lora_encode(&unknown, big, sizeof(big)) == 0);
}

static void decode_rejects_bad_input(void) {
    lora_msg_t out;
    uint8_t    buf[LORA_MSG_MAX];
    lora_msg_t cfg = {.type = LORA_MSG_CONFIG, .u.config = LORA_CONFIG_DEFAULT};
    size_t     n   = lora_encode(&cfg, buf, sizeof(buf));

    // Both chips must run the same protocol: older (v3, no sweep, #161) and newer images are not understood
    buf[0] = 3;
    CHECK(lora_decode(buf, n, &out) == LORA_DEC_BAD_VERSION);
    buf[0] = 5;
    CHECK(lora_decode(buf, n, &out) == LORA_DEC_BAD_VERSION);
    buf[0] = LORA_PROTO_VERSION;

    buf[1] = 0x55;
    CHECK(lora_decode(buf, n, &out) == LORA_DEC_BAD_TYPE);
    buf[1] = LORA_MSG_CONFIG;

    CHECK(lora_decode(buf, n - 1, &out) == LORA_DEC_BAD_LEN);
    buf[n] = 0;
    CHECK(lora_decode(buf, n + 1, &out) == LORA_DEC_BAD_LEN);

    // RX_PACKET whose length byte claims one more data byte than follows
    lora_msg_t rx = {.type = LORA_MSG_RX_PACKET};
    rx.u.rx.len   = 10;
    n             = lora_encode(&rx, buf, sizeof(buf));
    CHECK(lora_decode(buf, n - 1, &out) == LORA_DEC_BAD_LEN);

    // Empty packets are never sent
    buf[10] = 0;
    CHECK(lora_decode(buf, 11, &out) == LORA_DEC_BAD_LEN);

    CHECK(lora_decode(buf, 0, &out) == LORA_DEC_BAD_LEN);
    CHECK(lora_decode(buf, 1, &out) == LORA_DEC_BAD_LEN);

    // Bodyless commands carry no extra bytes
    uint8_t start[] = {LORA_PROTO_VERSION, LORA_MSG_RX_START, 0};
    CHECK(lora_decode(start, 3, &out) == LORA_DEC_BAD_LEN);

    // A SEND's length byte must match what follows, and be 1-255
    uint8_t send[] = {LORA_PROTO_VERSION, LORA_MSG_SEND, 2, 0xAA, 0xBB};
    CHECK(lora_decode(send, sizeof(send), &out) == LORA_DEC_OK && out.u.tx.len == 2 && out.u.tx.data[1] == 0xBB);
    CHECK(lora_decode(send, sizeof(send) - 1, &out) == LORA_DEC_BAD_LEN);
    uint8_t send_empty[] = {LORA_PROTO_VERSION, LORA_MSG_SEND, 0};
    CHECK(lora_decode(send_empty, sizeof(send_empty), &out) == LORA_DEC_BAD_LEN);
    CHECK(lora_decode(send, 2, &out) == LORA_DEC_BAD_LEN);
    uint8_t send_max[3 + 255] = {LORA_PROTO_VERSION, LORA_MSG_SEND, 255};
    CHECK(lora_decode(send_max, sizeof(send_max), &out) == LORA_DEC_OK && out.u.tx.len == 255);
}

// IDENT (#36): the P4 asks the running C6 app for its ELF sha256, so a warm boot can trust what the C6 flash holds
static void ident_round_trip(void) {
    size_t     n;
    uint8_t    buf[LORA_MSG_MAX];
    lora_msg_t out;

    lora_msg_t req = {.type = LORA_MSG_IDENT_REQ};
    round_trip(&req, &n);
    CHECK(n == 2);
    uint8_t req_long[] = {LORA_PROTO_VERSION, LORA_MSG_IDENT_REQ, 0};
    CHECK(lora_decode(req_long, sizeof(req_long), &out) == LORA_DEC_BAD_LEN);

    lora_msg_t id = {.type = LORA_MSG_IDENT};
    for (int i = 0; i < LORA_IDENT_SHA_LEN; i++) {
        id.u.ident.app_elf_sha256[i] = (uint8_t)(0xF0 - i);
    }
    lora_msg_t got = round_trip(&id, &n);
    CHECK(n == 2 + LORA_IDENT_SHA_LEN);
    CHECK(memcmp(got.u.ident.app_elf_sha256, id.u.ident.app_elf_sha256, LORA_IDENT_SHA_LEN) == 0);

    // the sha follows the header byte for byte
    CHECK(lora_encode(&id, buf, sizeof(buf)) == n);
    CHECK(buf[0] == LORA_PROTO_VERSION && buf[1] == LORA_MSG_IDENT && buf[2] == 0xF0 && buf[n - 1] == 0xF0 - 31);
    CHECK(lora_encode(&id, buf, n - 1) == 0); // too small
    CHECK(lora_decode(buf, n - 1, &out) == LORA_DEC_BAD_LEN);
    buf[n] = 0;
    CHECK(lora_decode(buf, n + 1, &out) == LORA_DEC_BAD_LEN);
    buf[0] = LORA_PROTO_VERSION + 1;
    CHECK(lora_decode(buf, n, &out) == LORA_DEC_BAD_VERSION);
}

static void config_valid_ranges(void) {
    lora_config_t const def = LORA_CONFIG_DEFAULT;
    lora_config_t       c   = def;
    CHECK(lora_config_valid(&c));

    struct {
        int      field;
        uint32_t value;
        bool     valid;
    } cases[] = {
        {0, 433000000, false},
        {0, 862000000, true},
        {0, 928000000, true},
        {0, 928000001, false},
        {1, 60000, false},
        {1, 500000, true},
        {1, 7800, true},
        {1, 41700, true},
        {2, 4, false},
        {2, 5, true},
        {2, 12, true},
        {2, 13, false},
        {3, 4, false},
        {3, 8, true},
        {3, 9, false},
        {4, 5, false},
        {4, 6, true},
        {5, (uint32_t)-10, false},
        {5, (uint32_t)-9, true},
        {5, 22, true},
        {5, 23, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        c = def;
        switch (cases[i].field) {
            case 0: c.freq_hz = cases[i].value; break;
            case 1: c.bw_hz = cases[i].value; break;
            case 2: c.sf = (uint8_t)cases[i].value; break;
            case 3: c.cr = (uint8_t)cases[i].value; break;
            case 4: c.preamble = (uint16_t)cases[i].value; break;
            case 5: c.tx_power_dbm = (int8_t)(int32_t)cases[i].value; break;
        }
        if (lora_config_valid(&c) != cases[i].valid) {
            printf("FAIL config case %zu (field %d value %u)\n", i, cases[i].field, (unsigned)cases[i].value);
            failures++;
        }
    }
}

// RSSI sweep (#161): request and answer round trip, the layout pinned, every bad length refused
static void sweep_round_trip(void) {
    size_t     n;
    uint8_t    buf[LORA_MSG_MAX];
    lora_msg_t out;

    lora_msg_t req = {.type = LORA_MSG_SWEEP_REQ};
    req.u.sweep_req = (lora_sweep_req_t){.start_hz = 868000000, .step_hz = 62500, .count = 32, .samples = 4, .seq = 7};
    lora_msg_t got = round_trip(&req, &n);
    CHECK(n == 13);
    CHECK(got.u.sweep_req.start_hz == 868000000 && got.u.sweep_req.step_hz == 62500);
    CHECK(got.u.sweep_req.count == 32 && got.u.sweep_req.samples == 4 && got.u.sweep_req.seq == 7);
    CHECK(lora_encode(&req, buf, sizeof(buf)) == 13);
    uint8_t const req_bytes[] = {LORA_PROTO_VERSION, LORA_MSG_SWEEP_REQ, 0x00, 0xA1, 0xBC, 0x33, 0x24, 0xF4, 0, 0, 32, 4, 7};
    CHECK(memcmp(buf, req_bytes, sizeof(req_bytes)) == 0);
    CHECK(lora_decode(buf, 12, &out) == LORA_DEC_BAD_LEN);

    lora_msg_t sw = {.type = LORA_MSG_SWEEP};
    sw.u.sweep.start_hz         = 863000000;
    sw.u.sweep.step_hz          = 62500;
    sw.u.sweep.seq              = 200;
    sw.u.sweep.channel_rssi_x10 = -1125;
    sw.u.sweep.count            = LORA_SWEEP_MAX_BINS;
    for (int i = 0; i < LORA_SWEEP_MAX_BINS; i++) {
        sw.u.sweep.rssi_x10[i] = (int16_t)(-1300 + 7 * i);
    }
    got = round_trip(&sw, &n);
    CHECK(n == 270 && n <= LORA_MSG_MAX);
    CHECK(got.u.sweep.start_hz == 863000000 && got.u.sweep.step_hz == 62500 && got.u.sweep.seq == 200);
    CHECK(got.u.sweep.channel_rssi_x10 == -1125 && got.u.sweep.count == LORA_SWEEP_MAX_BINS);
    CHECK(memcmp(got.u.sweep.rssi_x10, sw.u.sweep.rssi_x10, sizeof(sw.u.sweep.rssi_x10)) == 0);

    sw.u.sweep.count = 2;
    n                = lora_encode(&sw, buf, sizeof(buf));
    CHECK(n == 18);
    // count, then the steps little-endian: -1300 = 0xFAEC, -1293 = 0xFAF3
    CHECK(buf[13] == 2 && buf[14] == 0xEC && buf[15] == 0xFA && buf[16] == 0xF3 && buf[17] == 0xFA);
    CHECK(lora_decode(buf, n - 1, &out) == LORA_DEC_BAD_LEN);
    CHECK(lora_decode(buf, n + 1, &out) == LORA_DEC_BAD_LEN);
    CHECK(lora_decode(buf, 13, &out) == LORA_DEC_BAD_LEN);
    buf[13] = 0; // no steps
    CHECK(lora_decode(buf, 14, &out) == LORA_DEC_BAD_LEN);
    buf[13] = LORA_SWEEP_MAX_BINS + 1;
    CHECK(lora_decode(buf, 14 + 2 * (LORA_SWEEP_MAX_BINS + 1), &out) == LORA_DEC_BAD_LEN);

    sw.u.sweep.count = 0;
    CHECK(lora_encode(&sw, buf, sizeof(buf)) == 0);
    sw.u.sweep.count = LORA_SWEEP_MAX_BINS + 1;
    CHECK(lora_encode(&sw, buf, sizeof(buf)) == 0);
}

static void sweep_valid_ranges(void) {
    lora_sweep_req_t const ok = {.start_hz = 868000000, .step_hz = 62500, .count = 32, .samples = 4};
    lora_sweep_req_t       r  = ok;
    CHECK(lora_sweep_req_valid(&r));
    r.count = 0;
    CHECK(!lora_sweep_req_valid(&r));
    r.count = LORA_SWEEP_MAX_BINS;
    CHECK(lora_sweep_req_valid(&r));
    r.count = LORA_SWEEP_MAX_BINS + 1;
    CHECK(!lora_sweep_req_valid(&r));
    r = ok, r.samples = 0;
    CHECK(!lora_sweep_req_valid(&r));
    r.samples = LORA_SWEEP_MAX_SAMPLES;
    CHECK(lora_sweep_req_valid(&r));
    r.samples = LORA_SWEEP_MAX_SAMPLES + 1;
    CHECK(!lora_sweep_req_valid(&r));
    r = ok, r.step_hz = LORA_SWEEP_MIN_STEP_HZ - 1;
    CHECK(!lora_sweep_req_valid(&r));
    r.step_hz = LORA_SWEEP_MAX_STEP_HZ + 1;
    CHECK(!lora_sweep_req_valid(&r));
    r = ok, r.start_hz = 861999999;
    CHECK(!lora_sweep_req_valid(&r));
    // the last step decides the top: 927.9375 + 0.0625 = 928 MHz is in, one hertz more is not
    r = ok, r.start_hz = 928000000 - 31 * 62500;
    CHECK(lora_sweep_req_valid(&r));
    r.start_hz++;
    CHECK(!lora_sweep_req_valid(&r));
    // no wrap past 32 bits
    r = ok, r.start_hz = 0xFFFFFF00u, r.count = 2;
    CHECK(!lora_sweep_req_valid(&r));
}

// The registry messages (#308): round trip, exact bytes, exact length
static void reg_round_trip(void) {
    size_t     n;
    uint8_t    buf[LORA_MSG_MAX];
    lora_msg_t out;

    lora_msg_t get = {.type = LORA_MSG_REG_GET, .u.reg_get = {.id = 0x0207, .seq = 9}};
    lora_msg_t got = round_trip(&get, &n);
    CHECK(n == 5 && got.u.reg_get.id == 0x0207 && got.u.reg_get.seq == 9);

    lora_msg_t set = {.type = LORA_MSG_REG_SET, .u.reg_set = {.id = 0x0100, .value = 0xFFFFFFF7u, .seq = 200}};
    got            = round_trip(&set, &n);
    CHECK(n == 9 && got.u.reg_set.id == 0x0100 && got.u.reg_set.value == 0xFFFFFFF7u && got.u.reg_set.seq == 200);

    lora_msg_t val = {.type      = LORA_MSG_REG_VAL,
                      .u.reg_val = {.id = 0x0200, .seq = 7, .status = 3, .type = 3, .value = 869618000}};
    got            = round_trip(&val, &n);
    CHECK(n == 11 && got.u.reg_val.id == 0x0200 && got.u.reg_val.seq == 7 && got.u.reg_val.status == 3 &&
          got.u.reg_val.type == 3 && got.u.reg_val.value == 869618000);

    // Little-endian in struct order: [ver][type] id seq status type value
    uint8_t const expect[] = {0x04, 0x86, 0x00, 0x02, 0x07, 0x03, 0x03, 0x50, 0x51, 0xD5, 0x33};
    CHECK(lora_encode(&val, buf, sizeof(buf)) == sizeof(expect) && memcmp(buf, expect, sizeof(expect)) == 0);
    uint8_t const expect_set[] = {0x04, 0x09, 0x00, 0x01, 0xF7, 0xFF, 0xFF, 0xFF, 0xC8};
    CHECK(lora_encode(&set, buf, sizeof(buf)) == sizeof(expect_set) &&
          memcmp(buf, expect_set, sizeof(expect_set)) == 0);

    // Exact length: one byte short or long is BAD_LEN, a short buffer encodes nothing
    lora_msg_t const *each[] = {&get, &set, &val};
    for (size_t i = 0; i < 3; i++) {
        n = lora_encode(each[i], buf, sizeof(buf));
        CHECK(lora_decode(buf, n - 1, &out) == LORA_DEC_BAD_LEN);
        buf[n] = 0;
        CHECK(lora_decode(buf, n + 1, &out) == LORA_DEC_BAD_LEN);
        CHECK(lora_decode(buf, 2, &out) == LORA_DEC_BAD_LEN);
        CHECK(lora_encode(each[i], buf, n - 1) == 0);
    }
}

// No version bump for the registry (#308): an older peer keeps understanding every other message and drops a REG one.
// A v4 C6 without the registry decodes REG_GET / REG_SET as an unknown type, like any type past its table here.
static void reg_old_peer_drops(void) {
    uint8_t    buf[LORA_MSG_MAX];
    lora_msg_t out;
    lora_msg_t get = {.type = LORA_MSG_REG_GET, .u.reg_get = {.id = 0x0F00, .seq = 1}};
    size_t     n   = lora_encode(&get, buf, sizeof(buf));

    CHECK(LORA_PROTO_VERSION == 4 && buf[0] == 4);
    CHECK(LORA_MSG_REG_GET > LORA_MSG_SWEEP_REQ && LORA_MSG_REG_VAL > LORA_MSG_SWEEP); // new numbers, none reused
    buf[1] = LORA_MSG_REG_SET + 1; // the next type nobody knows yet: what REG_GET is to an older C6
    CHECK(lora_decode(buf, n, &out) == LORA_DEC_BAD_TYPE);
    buf[1] = LORA_MSG_REG_VAL + 1;
    CHECK(lora_decode(buf, n, &out) == LORA_DEC_BAD_TYPE);
    buf[1] = LORA_MSG_REG_GET;
    buf[0] = 3; // a v3 C6 drops it on the version, like every v4 message
    CHECK(lora_decode(buf, n, &out) == LORA_DEC_BAD_VERSION);
}

int main(void) {
    round_trip_each_type();
    encode_layout_pinned();
    encode_short_buffer();
    decode_rejects_bad_input();
    config_valid_ranges();
    ident_round_trip();
    sweep_round_trip();
    sweep_valid_ranges();
    reg_round_trip();
    reg_old_peer_drops();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("lora_proto_test: all passed\n");
    return 0;
}
