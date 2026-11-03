// Host test: signal history per contact (#166)
#include "mesh_signal.h"

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

static sig_table_t table;

static void key(uint8_t out[SIG_PREFIX], uint8_t n) {
    memset(out, n, SIG_PREFIX);
}

int main(void) {
    uint8_t a[SIG_PREFIX], b[SIG_PREFIX];
    key(a, 1);
    key(b, 2);
    sig_init(&table);
    sig_contact_t c;
    sig_sample_t  s[SIG_SAMPLES];
    CHECK(!sig_get(&table, a, &c)); // nothing heard: the empty state

    // an advert over two hops of 2-byte hashes
    uint8_t path[] = {0xA1, 0xB2, 0xC3, 0xD4};
    sig_note(&table, a, SIG_ADVERT, -874, 65, (uint8_t)((1 << 6) | 2), path, 100);
    CHECK(sig_get(&table, a, &c) && c.count == 1 && c.heard == 1 && c.have_path);
    CHECK(sig_samples(&c, s, SIG_SAMPLES) == 1 && s[0].hops == 2 && s[0].rssi_x10 == -874 && s[0].kind == SIG_ADVERT);
    char text[96];
    sig_path_text(&c, text, sizeof(text));
    CHECK_STR(text, "2 hops: a1b2,c3d4");
    sig_sample_text(&s[0], "3 min ago", text, sizeof(text));
    CHECK_STR(text, "-87 dBm, 6.5 dB SNR, 2 hops, advert, 3 min ago");

    // a message does not touch the advert path; direct = 0 hops
    sig_note(&table, a, SIG_MESSAGE, -1000, -35, 0, NULL, 160);
    CHECK(sig_get(&table, a, &c) && c.count == 2 && c.heard == 2 && c.path_hops == 2 && c.path_at_s == 100);
    CHECK(sig_samples(&c, s, SIG_SAMPLES) == 2 && s[0].at_s == 100 && s[1].at_s == 160 && s[1].hops == 0);
    sig_sample_text(&s[1], "just now", text, sizeof(text));
    CHECK_STR(text, "-100 dBm, -3.5 dB SNR, direct, message, just now");

    // a zero-hop advert replaces the path
    sig_note(&table, a, SIG_ADVERT, -900, 50, 0, NULL, 200);
    sig_get(&table, a, &c);
    sig_path_text(&c, text, sizeof(text));
    CHECK_STR(text, "direct (no hops)");
    // an invalid path_len (hash size 4) keeps the last good path
    sig_note(&table, a, SIG_ADVERT, -900, 50, (uint8_t)(3 << 6 | 1), path, 210);
    sig_get(&table, a, &c);
    CHECK(c.have_path && c.path_at_s == 200 && c.heard == 4);

    // the ring wraps: the newest 24 stay, oldest first
    for (uint32_t i = 0; i < 40; i++) {
        sig_note(&table, b, SIG_MESSAGE, (int16_t)(-800 - (int)i), 40, 0, NULL, 1000 + i);
    }
    CHECK(sig_get(&table, b, &c) && c.count == SIG_SAMPLES && c.heard == 40);
    CHECK(sig_samples(&c, s, SIG_SAMPLES) == SIG_SAMPLES && s[0].at_s == 1016 && s[SIG_SAMPLES - 1].at_s == 1039);
    CHECK(sig_samples(&c, s, 5) == 5 && s[0].at_s == 1035 && s[4].at_s == 1039);
    sig_heard_text(40, text, sizeof(text));
    CHECK_STR(text, "Heard 40 times since start");
    sig_heard_text(1, text, sizeof(text));
    CHECK_STR(text, "Heard once since start");

    // a long path keeps whole hashes, and says so
    uint8_t longp[20];
    for (int i = 0; i < 20; i++) {
        longp[i] = (uint8_t)i;
    }
    sig_note(&table, b, SIG_ADVERT, -800, 40, (uint8_t)((1 << 6) | 10), longp, 2000);
    sig_get(&table, b, &c);
    CHECK(c.path_hops == 10 && c.path_bytes == 16);
    sig_path_text(&c, text, sizeof(text));
    CHECK(strncmp(text, "10 hops: 0001,0203,", 19) == 0 && strstr(text, "0e0f,...") != NULL);

    // 32 contacts fit; the 33rd replaces the one heard longest ago (a, not refreshed since), keeping the recent ones
    sig_init(&table);
    for (uint8_t i = 1; i <= SIG_CONTACTS; i++) {
        uint8_t k[SIG_PREFIX];
        key(k, i);
        sig_note(&table, k, SIG_MESSAGE, -800, 40, 0, NULL, i);
    }
    uint8_t first[SIG_PREFIX], second[SIG_PREFIX], extra[SIG_PREFIX];
    key(first, 1);
    key(second, 2);
    key(extra, 99);
    sig_note(&table, first, SIG_MESSAGE, -800, 40, 0, NULL, 50); // contact 1 heard again: contact 2 is now the oldest
    sig_note(&table, extra, SIG_MESSAGE, -800, 40, 0, NULL, 51);
    CHECK(sig_get(&table, first, &c) && c.heard == 2);
    CHECK(!sig_get(&table, second, &c));
    CHECK(sig_get(&table, extra, &c) && c.heard == 1);

    // graph bounds: whole units, at least 10 apart
    sig_sample_t g[3] = {{.rssi_x10 = -874}, {.rssi_x10 = -921}, {.rssi_x10 = -650}};
    int lo, hi;
    sig_range(g, 3, SIG_RSSI, &lo, &hi);
    CHECK(lo == -92 && hi == -65);
    sig_range(g, 1, SIG_RSSI, &lo, &hi);
    CHECK(hi - lo == 10 && lo <= -87 && hi >= -87);
    sig_range(g, 0, SIG_SNR, &lo, &hi);
    CHECK(hi - lo == 10);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_signal_test: ok\n");
    return 0;
}
