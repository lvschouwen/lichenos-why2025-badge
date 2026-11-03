// Host test: the radio health page's texts (#157)
#include "mesh_health.h"

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

static char buf[96];

int main(void) {
    mh_fmt_dbm(-874, buf, sizeof(buf));
    CHECK_STR(buf, "-87 dBm");
    mh_fmt_dbm(-875, buf, sizeof(buf));
    CHECK_STR(buf, "-88 dBm");
    mh_fmt_dbm(0, buf, sizeof(buf));
    CHECK_STR(buf, "0 dBm");

    mh_fmt_db(65, buf, sizeof(buf));
    CHECK_STR(buf, "6.5 dB");
    mh_fmt_db(-30, buf, sizeof(buf));
    CHECK_STR(buf, "-3.0 dB");
    mh_fmt_db(-5, buf, sizeof(buf));
    CHECK_STR(buf, "-0.5 dB");

    mh_fmt_air(0, 100, buf, sizeof(buf));
    CHECK_STR(buf, "0 ms (0%)");
    mh_fmt_air(480, 480, buf, sizeof(buf));
    CHECK_STR(buf, "480 ms (0.1%)");
    mh_fmt_air(4200, 1000, buf, sizeof(buf));
    CHECK_STR(buf, "4.2 s (0.4%)");
    mh_fmt_air(75000, 3600, buf, sizeof(buf));
    CHECK_STR(buf, "75 s (2.0%)");
    mh_fmt_air(180000, 3600, buf, sizeof(buf));
    CHECK_STR(buf, "3 min (5.0%)");
    mh_fmt_air(5, 100000, buf, sizeof(buf));
    CHECK_STR(buf, "5 ms (<0.1%)");
    mh_fmt_air(500, 0, buf, sizeof(buf)); // no uptime yet: no division
    CHECK_STR(buf, "500 ms (0%)");

    mh_fmt_uptime(42, buf, sizeof(buf));
    CHECK_STR(buf, "42 s");
    mh_fmt_uptime(180, buf, sizeof(buf));
    CHECK_STR(buf, "3 min");
    mh_fmt_uptime(7500, buf, sizeof(buf));
    CHECK_STR(buf, "2 h 5 min");
    mh_fmt_uptime(86400 + 3 * 3600, buf, sizeof(buf));
    CHECK_STR(buf, "1 d 3 h");

    mh_fmt_errors(0, buf, sizeof(buf));
    CHECK_STR(buf, "none");
    mh_fmt_errors(MH_ERR_FULL | MH_ERR_STARTRX, buf, sizeof(buf));
    CHECK_STR(buf, "queue full, rx timeout");
    mh_fmt_errors(0xFFF8, buf, sizeof(buf)); // unknown bits are not words
    CHECK_STR(buf, "none");

    mh_fmt_split(12, 3, buf, sizeof(buf));
    CHECK_STR(buf, "12 flood, 3 direct");

    mesh_health_t h = {0};
    mh_fmt_drops(&h, buf, sizeof(buf));
    CHECK_STR(buf, "none");
    h.rx_dropped = 2, h.send_failed = 1, h.recv_errors = 4;
    mh_fmt_drops(&h, buf, sizeof(buf));
    CHECK_STR(buf, "2 dropped, 1 failed send, 4 bad");
    h.send_failed = 3;
    mh_fmt_drops(&h, buf, 12); // a short buffer is cut, never overrun
    CHECK(strlen(buf) < 12);

    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("mesh_health_test ok\n");
    return 0;
}
