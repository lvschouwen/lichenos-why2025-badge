// Host test: telemetry request to a contact (#162): LPP values, the 30 s rate limit, frames, and the request's states
#include "mesh_telem.h"

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

static uint8_t const A[6] = {1, 2, 3, 4, 5, 6}, B[6] = {9, 9, 9, 9, 9, 9};

static void lpp(void) {
    tl_line_t l[TL_LINES_MAX];
    // MeshCore's own telemetry: battery 4.12 V and temperature 21.5 C on channel 1 (CayenneLPP bytes), a negative one
    uint8_t d[] = {1, 116, 0x01, 0x9C, 1, 103, 0x00, 0xD7, 2, 103, 0xFF, 0x9C};
    size_t  n   = tl_lpp_lines(d, sizeof(d), l, TL_LINES_MAX);
    CHECK(n == 3);
    CHECK(strcmp(l[0].text, "Battery: 4.12 V") == 0);
    CHECK(strcmp(l[1].text, "Temperature: 21.5 \xC2\xB0""C") == 0);
    CHECK(strcmp(l[2].text, "Temperature (channel 2): -10.0 \xC2\xB0""C") == 0);
    // GPS: 52.4620, 4.8833, 3.00 m
    uint8_t g[] = {1, 136, 0x08, 0x01, 0x4C, 0x00, 0xBE, 0xC1, 0x00, 0x01, 0x2C};
    CHECK(tl_lpp_lines(g, sizeof(g), l, TL_LINES_MAX) == 1);
    CHECK(strcmp(l[0].text, "Location: 52.4620, 4.8833, 3 m") == 0);
    // negative longitude
    uint8_t g2[] = {3, 136, 0x00, 0x00, 0x0A, 0xFF, 0xFF, 0xF6, 0x00, 0x00, 0x64};
    CHECK(tl_lpp_lines(g2, sizeof(g2), l, TL_LINES_MAX) == 1);
    CHECK(strcmp(l[0].text, "Location (channel 3): 0.0010, -0.0010, 1 m") == 0);
    // humidity 0.5 % steps, pressure 0.1 hPa
    uint8_t h[] = {4, 104, 101, 4, 115, 0x27, 0x96};
    CHECK(tl_lpp_lines(h, sizeof(h), l, TL_LINES_MAX) == 2);
    CHECK(strcmp(l[0].text, "Humidity (channel 4): 50.5 %") == 0);
    CHECK(strcmp(l[1].text, "Pressure (channel 4): 1013.4 hPa") == 0);
    // a cut value and an unknown type end the list with a line that says so
    uint8_t cut[] = {1, 116, 0x01, 0x9C, 1, 103, 0x00};
    CHECK(tl_lpp_lines(cut, sizeof(cut), l, TL_LINES_MAX) == 2 && strstr(l[1].text, "cut off"));
    uint8_t unk[] = {1, 116, 0x01, 0x9C, 1, 77, 0x00, 0x00};
    CHECK(tl_lpp_lines(unk, sizeof(unk), l, TL_LINES_MAX) == 2 && strstr(l[1].text, "type 77"));
    CHECK(tl_lpp_lines(d, 0, l, TL_LINES_MAX) == 0);
    CHECK(tl_lpp_lines(d, sizeof(d), l, 2) == 2); // never past max
}

static void rate(void) {
    tl_rate_t t;
    tl_rate_init(&t);
    CHECK(tl_rate_wait_s(&t, A, 1000) == 0);
    tl_rate_note(&t, A, 1000);
    CHECK(tl_rate_wait_s(&t, A, 1000) == 30);
    CHECK(tl_rate_wait_s(&t, A, 1001) == 30); // rounded up
    CHECK(tl_rate_wait_s(&t, A, 1000 + 29000) == 1);
    CHECK(tl_rate_wait_s(&t, A, 1000 + 30000) == 0);
    CHECK(tl_rate_wait_s(&t, B, 1000) == 0); // per contact
    // the clock wraps
    tl_rate_note(&t, B, 0xFFFFFF00u);
    CHECK(tl_rate_wait_s(&t, B, 0xFFFFFF00u + 5000) == 25);
    // a full table reuses the oldest slot, never forgets a recent one
    tl_rate_init(&t);
    for (uint8_t i = 0; i < TL_RATE_SLOTS; i++) {
        uint8_t p[6] = {i, 1, 1, 1, 1, 1};
        tl_rate_note(&t, p, 100u * (i + 1));
    }
    uint8_t extra[6] = {200, 1, 1, 1, 1, 1}, first[6] = {0, 1, 1, 1, 1, 1}, last[6] = {TL_RATE_SLOTS - 1, 1, 1, 1, 1, 1};
    tl_rate_note(&t, extra, 5000);
    CHECK(tl_rate_wait_s(&t, first, 5000) == 0);
    CHECK(tl_rate_wait_s(&t, last, 5000) > 0 && tl_rate_wait_s(&t, extra, 5000) == 30);
}

static void frames(void) {
    uint8_t pub[32], f[MF_MAX_FRAME];
    for (int i = 0; i < 32; i++) {
        pub[i] = (uint8_t)i;
    }
    CHECK(mf_encode_telemetry_req(f, pub) == 36 && f[0] == 39 && f[1] == 0 && f[3] == 0 && f[4] == 0 && f[35] == 31);
    uint8_t  push[] = {0x8B, 0, 1, 2, 3, 4, 5, 6, 1, 116, 0x01, 0x9C};
    mf_msg_t m;
    mf_decode(push, sizeof(push), &m);
    CHECK(m.kind == MF_TELEMETRY && m.telem_len == 4 && m.telem[1] == 116 && m.rprefix[5] == 6);
    mf_decode(push, 7, &m);
    CHECK(m.kind == MF_BAD);
}

static void request(void) {
    rr_t        r;
    char const *why = NULL;
    rr_init(&r);
    uint32_t seq = rr_request(&r, RR_TELEMETRY, A, NULL, 100, &why);
    CHECK(seq == 1 && rr_busy(&r));
    rr_op_t op;
    uint8_t prefix[6];
    char    arg[RR_ARG_MAX + 1];
    CHECK(rr_take(&r, &op, prefix, arg, sizeof(arg), 100) && op == RR_TELEMETRY);
    mf_msg_t sent = {.kind = MF_SENT, .flood = 0, .est_timeout = 4000};
    rr_on_reply(&r, &sent, 150);
    CHECK(r.state == RR_SENT);
    char text[128];
    rr_state_text(&r, text, sizeof(text));
    CHECK(strstr(text, "telemetry") && strstr(text, "waiting"));
    // an answer from someone else is not ours
    mf_msg_t ans = {.kind = MF_TELEMETRY, .telem_len = 4};
    memcpy(ans.rprefix, B, 6);
    CHECK(!rr_on_telemetry(&r, &ans, 200));
    memcpy(ans.rprefix, A, 6);
    uint8_t d[] = {1, 116, 0x01, 0x9C};
    memcpy(ans.telem, d, 4);
    CHECK(rr_on_telemetry(&r, &ans, 300) && r.state == RR_DONE && r.have_telem);
    char view[600];
    tl_view_text(&r, A, view, sizeof(view));
    CHECK(strcmp(view, "Battery: 4.12 V") == 0); // a good answer: values only, no state line
    // the next request times out: "No answer", the old values stay above with the reason
    CHECK(rr_request(&r, RR_TELEMETRY, A, NULL, 1000, &why) == 2);
    CHECK(rr_take(&r, &op, prefix, arg, sizeof(arg), 1000));
    rr_on_reply(&r, &sent, 1100);
    rr_poll(&r, 1100 + rr_deadline_ms(4000) - 1, NULL);
    CHECK(r.state == RR_SENT);
    rr_poll(&r, 1100 + rr_deadline_ms(4000) + 1, NULL);
    CHECK(r.state == RR_TIMEOUT);
    tl_view_text(&r, A, view, sizeof(view));
    CHECK(strstr(view, "Battery: 4.12 V") && strstr(view, "No answer:") && strstr(view, "last answer above"));
    tl_view_text(&r, B, view, sizeof(view)); // another contact's page never shows these values
    CHECK(strncmp(view, "No answer:", 10) == 0);
    // MyMesh refusing it ends it with the reason
    CHECK(rr_request(&r, RR_TELEMETRY, A, NULL, 9000, &why) == 3);
    CHECK(rr_take(&r, &op, prefix, arg, sizeof(arg), 9000));
    mf_msg_t err = {.kind = MF_ERR, .err = MF_ERR_NOT_FOUND};
    rr_on_reply(&r, &err, 9010);
    CHECK(r.state == RR_FAILED && strstr(r.why, "not found"));
}

int main(void) {
    lpp();
    rate();
    frames();
    request();
    if (failures) {
        printf("mesh_telem_test: %d failures\n", failures);
        return 1;
    }
    printf("mesh_telem_test: ok\n");
    return 0;
}
