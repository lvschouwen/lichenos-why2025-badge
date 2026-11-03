// Host test: the air log's ring and row texts (#165)
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

int main(void) {
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
