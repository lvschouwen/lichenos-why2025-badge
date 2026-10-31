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
    uint8_t const expect[] = {0x03, 0x01, 0x50, 0x51, 0xD5, 0x33, 0x24, 0xF4, 0x00,
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

    // Both chips must run the same protocol: older (v2, receive only) and newer images are not understood
    buf[0] = 2;
    CHECK(lora_decode(buf, n, &out) == LORA_DEC_BAD_VERSION);
    buf[0] = 4;
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

int main(void) {
    round_trip_each_type();
    encode_layout_pinned();
    encode_short_buffer();
    decode_rejects_bad_input();
    config_valid_ranges();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("lora_proto_test: all passed\n");
    return 0;
}
