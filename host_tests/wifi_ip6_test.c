// Host tests for the station's IPv6 addresses (#497): address classes, the set behind wifi.ip6 (none / linklocal /
// global), the address shown and its RFC 5952 text.
#include "wifi_ip6.h"

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

static uint8_t const LL[16]  = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0x1a, 0x2b, 0xff, 0xfe, 0x3c, 0x4d, 0x5e};
static uint8_t const GUA[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
static uint8_t const GUA2[16] = {0x24, 0x00, 0x12, 0x34, 0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6};
static uint8_t const ULA[16] = {0xfd, 0x12, 0x34, 0x56, 0x78, 0x9a, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0x07};

static void test_classify(void) {
    uint8_t any[16] = {0};
    uint8_t lo[16]  = {0};
    lo[15]          = 1;
    uint8_t mc[16]  = {0xff, 0x02};
    uint8_t fec0[16] = {0xfe, 0xc0}; // site-local, deprecated: not a link-local, not global
    uint8_t fe80[16] = {0xfe, 0xbf}; // still inside fe80::/10
    CHECK(wifi_ip6_classify(LL) == WIFI_IP6_CLASS_LINKLOCAL);
    CHECK(wifi_ip6_classify(fe80) == WIFI_IP6_CLASS_LINKLOCAL);
    CHECK(wifi_ip6_classify(fec0) == WIFI_IP6_CLASS_OTHER);
    CHECK(wifi_ip6_classify(GUA) == WIFI_IP6_CLASS_GLOBAL && wifi_ip6_classify(GUA2) == WIFI_IP6_CLASS_GLOBAL);
    CHECK(wifi_ip6_classify(ULA) == WIFI_IP6_CLASS_ULA);
    CHECK(wifi_ip6_classify(any) == WIFI_IP6_CLASS_OTHER && wifi_ip6_classify(lo) == WIFI_IP6_CLASS_OTHER);
    CHECK(wifi_ip6_classify(mc) == WIFI_IP6_CLASS_OTHER);
}

static void test_level(void) {
    wifi_ip6_set_t s;
    uint8_t        out[16];
    wifi_ip6_clear(&s);
    CHECK(wifi_ip6_level(&s) == WIFI_IP6_NONE && wifi_ip6_global_count(&s) == 0 && !wifi_ip6_best(&s, out));
    CHECK(wifi_ip6_add(&s, LL));
    CHECK(!wifi_ip6_add(&s, LL)); // twice is one
    CHECK(wifi_ip6_level(&s) == WIFI_IP6_LINKLOCAL && !wifi_ip6_best(&s, out));
    uint8_t mc[16] = {0xff, 0x02};
    CHECK(!wifi_ip6_add(&s, mc) && s.n == 1);
    CHECK(wifi_ip6_add(&s, ULA));
    CHECK(wifi_ip6_level(&s) == WIFI_IP6_GLOBAL && wifi_ip6_global_count(&s) == 1);
    CHECK(wifi_ip6_best(&s, out) && memcmp(out, ULA, 16) == 0); // a ULA is shown when nothing better is there
    CHECK(wifi_ip6_add(&s, GUA));
    CHECK(wifi_ip6_best(&s, out) && memcmp(out, GUA, 16) == 0 && wifi_ip6_global_count(&s) == 2); // the global wins
    CHECK(wifi_ip6_add(&s, GUA2));
    CHECK(s.n == 4 && wifi_ip6_global_count(&s) == 3);
    wifi_ip6_clear(&s);
    CHECK(wifi_ip6_level(&s) == WIFI_IP6_NONE);
}

static void test_full(void) {
    wifi_ip6_set_t s;
    wifi_ip6_clear(&s);
    for (int i = 0; i < WIFI_IP6_MAX; i++) {
        uint8_t a[16] = {0x20, 0x01};
        a[15]         = (uint8_t)(i + 1);
        CHECK(wifi_ip6_add(&s, a));
    }
    uint8_t extra[16] = {0x20, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    CHECK(!wifi_ip6_add(&s, extra) && s.n == WIFI_IP6_MAX);
}

static void text_is(uint8_t const *a, char const *want) {
    char   buf[WIFI_IP6_TEXT_MAX];
    size_t n = wifi_ip6_text(a, buf, sizeof(buf));
    if (strcmp(buf, want) != 0 || n != strlen(want)) {
        printf("FAIL text: got \"%s\" (%zu), want \"%s\"\n", buf, n, want);
        failures++;
    }
}

static void test_text(void) {
    text_is(GUA, "2001:db8::1");
    text_is(LL, "fe80::21a:2bff:fe3c:4d5e");
    text_is(GUA2, "2400:1234:1:2:3:4:5:6"); // no run of two zero groups
    text_is(ULA, "fd12:3456:789a:1::7");
    uint8_t zero[16] = {0}, lo[16] = {0}, tail[16] = {0xfe, 0x80}, lead[16] = {0};
    lo[15]   = 1;
    lead[2]  = 0; lead[3] = 1; // ::1:...
    lead[15] = 2;
    text_is(zero, "::");
    text_is(lo, "::1");
    text_is(tail, "fe80::");
    text_is(lead, "0:1::2"); // the longest run (5 groups) beats the single leading zero
    uint8_t single[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    text_is(single, "2001:db8:0:1:1:1:1:1"); // one zero group is written out (RFC 5952 4.2.2)
    uint8_t tie[16] = {0x20, 0x01, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 2, 0, 3}; // two runs of 2: the first
    text_is(tie, "2001::1:0:0:2:3");
    char pre[WIFI_IP6_TEXT_MAX + 3];
    uint8_t eui[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 1, 0, 2, 0x02, 0x11, 0x22, 0xff, 0xfe, 0x33, 0x44, 0x55};
    size_t  pn      = wifi_ip6_prefix_text(eui, pre, sizeof(pre)); // a log line must not carry the MAC in the low half
    CHECK(strcmp(pre, "2001:db8:1:2::/64") == 0 && pn == strlen(pre) && strstr(pre, "4455") == NULL);
    CHECK(wifi_ip6_prefix_text(eui, pre, 20) == 0);
    char small[8];
    CHECK(wifi_ip6_text(GUA, small, sizeof(small)) == 0);
}

int main(void) {
    test_classify();
    test_level();
    test_full();
    test_text();
    if (failures) {
        printf("wifi_ip6_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("wifi_ip6_test: ok\n");
    return 0;
}
