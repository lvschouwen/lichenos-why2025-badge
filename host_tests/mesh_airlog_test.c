// Host test: the air log's ring and row texts (#165), its transmits and console form (#408)
#include "mesh_airlog.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define CHECK_STR(got, want)                                                                                           \
    do {                                                                                                               \
        if (strcmp((got), (want)) != 0) {                                                                              \
            printf("FAIL %s:%d: got '%s' want '%s'\n", __FILE__, __LINE__, (got), (want));                             \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// header = version 0 | type << 2 | route
#define HDR(type, route) ((uint8_t)(((type) << 2) | (route)))

static airlog_entry_t store[4];

// Every payload type the receive side names gets a console token, and a sent packet's kind comes from its header the
// same way: request, response, text, ACK, advert, channel text and data, login (anonymous request), path, trace,
// multipart, control; the rest by number
static void kinds(void) {
    static char const *const want[] = {"req",     "resp",  "txt",       "ack",    "advert",  "grp_txt",  "grp_data",
                                       "anon_req", "path", "trace", "multipart", "control", "type12", "type15"};
    static uint8_t const     types[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 15};
    airlog_entry_t           mem[2];
    airlog_t                 r;
    airlog_entry_t           out[2];
    for (size_t i = 0; i < sizeof(types); i++) {
        CHECK_STR(airlog_kind_token(types[i]), want[i]);
        airlog_init(&r, mem, 2);
        airlog_push_tx(&r, HDR(types[i], 2), 0, 10, false, AIRLOG_TX_OK, 1);
        airlog_push(&r, HDR(types[i], 1), 0, NULL, 10, -900, 10, 2);
        CHECK(airlog_copy(&r, out, 2) == 2);
        CHECK(out[1].type == types[i] && airlog_is_sent(&out[1]) && !airlog_is_flood(&out[1]));
        CHECK(out[0].type == types[i] && !airlog_is_sent(&out[0]) && airlog_is_flood(&out[0]));
    }
}

// A sent packet keeps its hop count but never a hash, and its signal fields stay zero; its row says Sent
static void sent_entries(void) {
    airlog_entry_t mem[3];
    airlog_t       r;
    airlog_init(&r, mem, 3);
    airlog_push_tx(&r, HDR(2, 2), (uint8_t)((1 << 6) | 2), 45, true, AIRLOG_TX_OK, 50); // direct, 2 hops of 2 bytes
    airlog_entry_t out[3];
    CHECK(airlog_copy(&r, out, 3) == 1);
    CHECK(out[0].hops == 2 && out[0].hash_size == 0 && out[0].hash[0] == 0 && out[0].rssi_x10 == 0 && out[0].len == 45);
    CHECK((out[0].flags & AIRLOG_F_RETRY) && out[0].result == AIRLOG_TX_OK);
    char t[96];
    airlog_title(&out[0], t, sizeof(t));
    CHECK_STR(t, "Direct message, direct, 2 hops");
    airlog_detail(&out[0], "just now", t, sizeof(t));
    CHECK_STR(t, "Sent, retry, just now");
    airlog_push_tx(&r, HDR(4, 1), (uint8_t)(3 << 6 | 1), 100, false, AIRLOG_TX_TIMEOUT, 51); // a bad path: no hops
    airlog_copy(&r, out, 3);
    CHECK(out[0].hops == 0 && !(out[0].flags & AIRLOG_F_RETRY));
    airlog_detail(&out[0], "2 s ago", t, sizeof(t));
    CHECK_STR(t, "Sent, timeout, 2 s ago");
}

// Received and sent entries share one ring and one sequence: newest first, in the order they happened, across a wrap
static void interleaved_and_wrap(void) {
    airlog_entry_t mem[4];
    airlog_t       r;
    airlog_init(&r, mem, 4);
    for (uint32_t i = 0; i < 7; i++) { // rx, tx, rx, tx ...: 7 pushes into 4 slots
        if (i % 2) {
            airlog_push_tx(&r, HDR(3, 1), 0, 9, false, AIRLOG_TX_OK, 100 + i);
        } else {
            airlog_push(&r, HDR(4, 1), 0, NULL, 80, -900, 50, 100 + i);
        }
    }
    airlog_entry_t out[8];
    CHECK(airlog_copy(&r, out, 8) == 4 && r.seq == 7);
    for (int i = 0; i < 4; i++) {
        uint32_t seq = 7 - (uint32_t)i;
        CHECK(out[i].seq == seq && out[i].at_s == 99 + seq);
        CHECK(airlog_is_sent(&out[i]) == (seq % 2 == 0)); // the 2nd, 4th, 6th pushes were sends
    }
}

// The console form is a contract for bench scripts (#408): pinned byte for byte
static void console_format(void) {
    airlog_entry_t mem[4];
    airlog_t       r;
    airlog_init(&r, mem, 4);
    char    line[128];
    uint8_t path[6] = {0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a};
    airlog_console_header(0, 0, line, sizeof(line));
    CHECK_STR(line, "air log: empty");
    airlog_push(&r, HDR(4, 1), (uint8_t)((1 << 6) | 3), path, 90, -874, 65, 1234);
    airlog_push_tx(&r, HDR(2, 2), 1, 45, true, AIRLOG_TX_OK, 1240);
    airlog_push(&r, HDR(5, 0), 0, NULL, 50, -1205, -5, 1241); // transport flood, SNR -0.5
    airlog_push_tx(&r, HDR(4, 1), 0, 110, false, AIRLOG_TX_REFUSED, 1250);
    airlog_push_tx(&r, HDR(5, 1), 0, 60, false, AIRLOG_TX_FAILED, 1251);
    airlog_entry_t out[4];
    size_t         n = airlog_copy(&r, out, 4);
    airlog_console_header(n, r.seq, line, sizeof(line));
    CHECK_STR(line, "air log: 4 entries, newest first, 5 logged since boot");
    airlog_console_line(&out[0], line, sizeof(line));
    CHECK_STR(line, "#5 t=1251 tx grp_txt flood hops=0 len=60 retry=0 result=failed");
    airlog_console_line(&out[1], line, sizeof(line));
    CHECK_STR(line, "#4 t=1250 tx advert flood hops=0 len=110 retry=0 result=refused");
    airlog_console_line(&out[2], line, sizeof(line));
    CHECK_STR(line, "#3 t=1241 rx grp_txt flood hops=0 len=50 rssi=-120.5 snr=-0.5");
    airlog_console_line(&out[3], line, sizeof(line));
    CHECK_STR(line, "#2 t=1240 tx txt direct hops=1 len=45 retry=1 result=ok");
    airlog_init(&r, mem, 4);
    airlog_push(&r, HDR(4, 1), (uint8_t)((1 << 6) | 3), path, 90, -874, 65, 1234);
    airlog_copy(&r, out, 1);
    airlog_console_line(&out[0], line, sizeof(line)); // no hash on the console: it names a repeater
    CHECK_STR(line, "#1 t=1234 rx advert flood hops=3 len=90 rssi=-87.4 snr=6.5");
    airlog_console_header(1, r.seq, line, sizeof(line));
    CHECK_STR(line, "air log: 1 entry, newest first, 1 logged since boot");
    airlog_push_tx(&r, HDR(1, 2), 0, 20, false, AIRLOG_TX_TIMEOUT, 1300);
    airlog_copy(&r, out, 1);
    airlog_console_line(&out[0], line, sizeof(line));
    CHECK_STR(line, "#2 t=1300 tx resp direct hops=0 len=20 retry=0 result=timeout");
}

int main(void) {
    kinds();
    sent_entries();
    interleaved_and_wrap();
    console_format();
    airlog_t r;
    airlog_init(&r, store, 4);
    airlog_entry_t out[8];
    CHECK(airlog_copy(&r, out, 8) == 0);

    // a flood advert with a two-hop path of 2-byte hashes: the hash shown is the last repeater (the path's end)
    uint8_t path[] = {0xAA, 0xBB, 0x1F, 0x2E};
    airlog_push(&r, HDR(4, 1), (uint8_t)((1 << 6) | 2), path, 90, -874, 65, 100);
    CHECK(airlog_copy(&r, out, 8) == 1);
    CHECK(out[0].seq == 1 && out[0].type == 4 && out[0].route == 1 && out[0].hops == 2 && out[0].hash_size == 2);
    CHECK(out[0].hash[0] == 0x1F && out[0].hash[1] == 0x2E);
    char t[80];
    airlog_title(&out[0], t, sizeof(t));
    CHECK_STR(t, "Advert, flood, 2 hops");
    airlog_detail(&out[0], "3 min ago", t, sizeof(t));
    CHECK_STR(t, "-87 dBm, 6.5 dB SNR, via 1f2e, 3 min ago");

    // direct: the next hop is the path's start; one hop is singular
    uint8_t dpath[] = {0x42};
    airlog_push(&r, HDR(3, 2), 1, dpath, 12, -1000, -35, 101);
    airlog_copy(&r, out, 8);
    CHECK(out[0].seq == 2 && out[0].hash[0] == 0x42);
    airlog_title(&out[0], t, sizeof(t));
    CHECK_STR(t, "Ack, direct, 1 hop");
    airlog_detail(&out[0], "just now", t, sizeof(t));
    CHECK_STR(t, "-100 dBm, -3.5 dB SNR, next 42, just now");

    // an empty path: no hash, no "via"
    airlog_push(&r, HDR(5, 1), 0, NULL, 40, -900, 20, 102);
    airlog_copy(&r, out, 8);
    airlog_title(&out[0], t, sizeof(t));
    CHECK_STR(t, "Channel message, flood, no hops");
    airlog_detail(&out[0], "2 s ago", t, sizeof(t));
    CHECK_STR(t, "-90 dBm, 2.0 dB SNR, 2 s ago");

    // a path the header cannot hold (hash size 4, or more than 64 bytes) is recorded without one
    airlog_push(&r, HDR(2, 1), (uint8_t)(3 << 6 | 1), path, 20, -800, 0, 103);
    airlog_copy(&r, out, 8);
    CHECK(out[0].hops == 0 && out[0].hash_size == 0);
    airlog_push(&r, HDR(2, 1), (uint8_t)(2 << 6 | 30), path, 20, -800, 0, 104); // 90 bytes
    airlog_copy(&r, out, 8);
    CHECK(out[0].hops == 0);

    // the ring keeps the newest 4, newest first, and the sequence goes on
    CHECK(airlog_copy(&r, out, 8) == 4);
    CHECK(out[0].seq == 5 && out[1].seq == 4 && out[2].seq == 3 && out[3].seq == 2);
    CHECK(airlog_copy(&r, out, 2) == 2 && out[1].seq == 4);
    CHECK(r.seq == 5);

    CHECK_STR(airlog_type_name(0), "Request");
    CHECK_STR(airlog_type_name(11), "Control");
    CHECK_STR(airlog_type_name(15), "Type 15");

    // a ring without storage ignores pushes
    airlog_t none;
    airlog_init(&none, NULL, 0);
    airlog_push(&none, HDR(4, 1), 0, NULL, 10, 0, 0, 1);
    CHECK(airlog_copy(&none, out, 8) == 0);

    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("mesh_airlog_test ok\n");
    return 0;
}
