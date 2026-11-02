// Host test: trace path and path discovery (#53): the frames, the plan from a stored path, the request's states and
// the result lines
#include "mesh_frame.h"
#include "mesh_trace.h"

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

static uint8_t const PUB[32] = {0x2a, 0xe9, 0x6c, 0x65, 0x7d, 0x03};

static void frames(void) {
    uint8_t f[MF_MAX_FRAME];
    uint8_t const h[4] = {0xa1, 0xb2, 0xc3, 0xd4};
    size_t n = mf_encode_trace(f, 0x11223344, 0, 1, h, 4); // two 2-byte hashes
    CHECK(n == 14 && f[0] == 36 && f[1] == 0x44 && f[4] == 0x11 && f[5] == 0 && f[9] == 1 && memcmp(f + 10, h, 4) == 0);
    CHECK(mf_encode_trace(f, 1, 0, 1, h, 3) == 0); // not whole 2-byte hashes
    CHECK(mf_encode_trace(f, 1, 0, 0, h, 0) == 0); // nothing to trace
    uint8_t big[MF_TRACE_HASH_MAX + 2];
    memset(big, 1, sizeof(big));
    CHECK(mf_encode_trace(f, 1, 0, 0, big, 65) == 0);  // 65 hops: MyMesh refuses over 64
    CHECK(mf_encode_trace(f, 1, 0, 1, big, 128) == 138);
    CHECK(mf_encode_discovery(f, PUB) == 34 && f[0] == 52 && f[1] == 0 && f[2] == 0x2a && f[33] == PUB[31]);

    // PUSH_TRACE_DATA as MyMesh::onTraceRecv writes it: three 1-byte hops, an SNR each, then ours
    uint8_t p[] = {0x89, 0, 3, 0, 0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0, 0xa1, 0x2a, 0xa1, 28, (uint8_t)-10, 4, 38};
    mf_msg_t m;
    mf_decode(p, sizeof(p), &m);
    CHECK(m.kind == MF_TRACE && m.trace.tag == 0x11223344 && m.trace.hops == 3 && m.trace.hash_len == 3);
    CHECK(m.trace.hashes[1] == 0x2a && m.trace.snr_q[0] == 28 && m.trace.snr_q[1] == -10 && m.trace.final_snr_q == 38);
    mf_decode(p, sizeof(p) - 1, &m);
    CHECK(m.kind == MF_BAD);
    p[3] = 1; // 2-byte hashes: 3 bytes is not whole, the lengths no longer add up
    mf_decode(p, sizeof(p), &m);
    CHECK(m.kind == MF_BAD);

    // PUSH_PATH_DISCOVERY_RESPONSE: out path one 2-byte hash, in path two 1-byte hashes
    uint8_t d[] = {0x8D, 0, 0x2a, 0xe9, 0x6c, 0x65, 0x7d, 0x03, 0x41, 0xa1, 0xb2, 0x02, 0xc3, 0xd4};
    mf_decode(d, sizeof(d), &m);
    CHECK(m.kind == MF_DISCOVERY && m.disc.out_len == 0x41 && m.disc.out_path[1] == 0xb2 && m.disc.in_len == 2 &&
          m.disc.in_path[1] == 0xd4 && memcmp(m.disc.prefix, PUB, 6) == 0);
    mf_decode(d, sizeof(d) - 1, &m);
    CHECK(m.kind == MF_BAD);
    d[8] = 0xC1; // hash size 4: reserved
    mf_decode(d, sizeof(d), &m);
    CHECK(m.kind == MF_BAD);
}

static void plans(void) {
    mt_plan_t     p;
    uint8_t const path1[] = {0xa1, 0xb2};       // two 1-byte hashes
    uint8_t const path2[] = {0xa1, 0xa2, 0xb1, 0xb2}; // two 2-byte hashes

    CHECK(mt_plan(2, 0xFF, path1, PUB, 1, &p) != NULL); // flood: nothing to trace
    CHECK(mt_plan(1, 0, path1, PUB, 1, &p) != NULL);    // a companion next door: no repeater

    // A repeater two hops away: out, through it, back
    CHECK(mt_plan(2, 2, path1, PUB, 1, &p) == NULL);
    uint8_t const want1[] = {0xa1, 0xb2, 0x2a, 0xb2, 0xa1};
    CHECK(p.flags == 0 && p.hash_len == 5 && p.target == 2 && memcmp(p.hashes, want1, 5) == 0);

    // A companion behind those two repeaters: to the last one and back
    CHECK(mt_plan(1, 2, path1, PUB, 1, &p) == NULL);
    uint8_t const want2[] = {0xa1, 0xb2, 0xa1};
    CHECK(p.hash_len == 3 && p.target == -1 && memcmp(p.hashes, want2, 3) == 0);

    // 2-byte hashes are reversed hash by hash, not byte by byte
    CHECK(mt_plan(2, 0x42, path2, PUB, 1, &p) == NULL);
    uint8_t const want3[] = {0xa1, 0xa2, 0xb1, 0xb2, 0x2a, 0xe9, 0xb1, 0xb2, 0xa1, 0xa2};
    CHECK(p.flags == 1 && p.hash_len == 10 && p.target == 2 && memcmp(p.hashes, want3, 10) == 0);

    // A repeater in direct range: just it, with our hash size
    CHECK(mt_plan(2, 0, path1, PUB, 2, &p) == NULL);
    CHECK(p.flags == 1 && p.hash_len == 2 && p.target == 0 && p.hashes[0] == 0x2a && p.hashes[1] == 0xe9);
    CHECK(mt_plan(2, 0, path1, PUB, 1, &p) == NULL && p.flags == 0 && p.hash_len == 1);

    CHECK(mt_plan(2, 0x81, path1, PUB, 1, &p) != NULL); // 3-byte hashes: a trace has no such size
    uint8_t long_path[64] = {0};
    CHECK(mt_plan(2, 32, long_path, PUB, 1, &p) != NULL); // 65 hops
    CHECK(mt_plan(2, 31, long_path, PUB, 1, &p) == NULL && p.hash_len == 63);
}

static mf_msg_t sent(uint32_t tag, uint32_t est) {
    mf_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind        = MF_SENT;
    m.ack         = tag;
    m.est_timeout = est;
    return m;
}

static void states(void) {
    mt_t        t, copy;
    char const *why = NULL;
    mt_kind_t   kind;
    uint8_t     prefix[6];
    uint32_t    tag;
    mt_init(&t);
    CHECK(!mt_take(&t, &kind, prefix, &tag, 0));

    uint32_t seq = mt_request(&t, MT_TRACE, PUB, 0xCAFE, 1000, &why);
    CHECK(seq == 1 && t.state == MT_QUEUED);
    CHECK(mt_request(&t, MT_DISCOVER, PUB, 0, 1000, &why) == 0 && why != NULL); // one at a time
    CHECK(mt_take(&t, &kind, prefix, &tag, 1010) && kind == MT_TRACE && tag == 0xCAFE && t.state == MT_TAKEN);
    CHECK(!mt_take(&t, &kind, prefix, &tag, 1010));
    mf_msg_t s = sent(0xCAFE, 2000);
    mt_on_reply(&t, &s, 1020);
    CHECK(t.state == MT_SENT);
    mt_on_tx(&t, 0xBEEF, 1100); // someone else's trace passed on by us
    mt_on_tx(&t, 0xCAFE, 1200);
    CHECK(t.on_air_ms == 1200);

    mf_trace_t tr;
    memset(&tr, 0, sizeof(tr));
    tr.tag = 0xBEEF;
    CHECK(!mt_on_trace(&t, &tr, 2000)); // not ours
    tr.tag         = 0xCAFE;
    tr.hops        = 3;
    tr.hash_len    = 3;
    tr.hashes[0]   = 0xa1;
    tr.hashes[1]   = 0x2a;
    tr.hashes[2]   = 0xa1;
    tr.snr_q[0]    = 29;
    tr.snr_q[1]    = -10;
    tr.snr_q[2]    = 0;
    tr.final_snr_q = 38;
    mt_set_target(&t, 1);
    CHECK(mt_on_trace(&t, &tr, 2434) && t.state == MT_DONE && mt_round_trip_ms(&t) == 1234);
    CHECK(!mt_busy(&t));

    mt_line_t lines[MT_LINES_MAX];
    size_t    n = mt_format(&t, lines, MT_LINES_MAX);
    CHECK(n == 5);
    CHECK(strcmp(lines[0].text, "1. a1  SNR 7.25 dB") == 0);
    CHECK(strcmp(lines[1].text, "2. 2a (contact)  SNR -2.5 dB") == 0);
    CHECK(strcmp(lines[3].text, "Back here: SNR 9.5 dB") == 0);
    CHECK(strcmp(lines[4].text, "Round trip: 1234 ms") == 0);
    CHECK(mt_format(&t, lines, 2) == 2); // cut to max

    // No answer: times out after MyMesh's estimate plus the grace, never before the floor
    CHECK(mt_request(&t, MT_TRACE, PUB, 7, 5000, &why) == 2);
    CHECK(mt_take(&t, &kind, prefix, &tag, 5000));
    s = sent(7, 100);
    mt_on_reply(&t, &s, 5000);
    mt_poll(&t, 5000 + MT_FLOOR_MS - 1, &copy);
    CHECK(copy.state == MT_SENT);
    n = mt_format(&copy, lines, MT_LINES_MAX);
    CHECK(n == 1 && strstr(lines[0].text, "up to 5 s") != NULL);
    mt_poll(&t, 5000 + MT_FLOOR_MS, &copy);
    CHECK(copy.state == MT_TIMEOUT && t.state == MT_TIMEOUT);
    tr.tag = 7;
    CHECK(!mt_on_trace(&t, &tr, 11000)); // late: stays timed out
    CHECK(mt_deadline_ms(10000) == 13000 && mt_deadline_ms(0) == MT_FLOOR_MS && mt_deadline_ms(UINT32_MAX) == MT_CAP_MS);

    // MyMesh refuses: failed with the reason
    CHECK(mt_request(&t, MT_TRACE, PUB, 8, 0xFFFFFF00u, &why) == 3);
    CHECK(mt_take(&t, &kind, prefix, &tag, 0xFFFFFF00u));
    mf_msg_t err;
    memset(&err, 0, sizeof(err));
    err.kind = MF_ERR;
    err.err  = MF_ERR_TABLE_FULL;
    mt_on_reply(&t, &err, 0xFFFFFF10u);
    CHECK(t.state == MT_FAILED && t.err == MF_ERR_TABLE_FULL);
    n = mt_format(&t, lines, MT_LINES_MAX);
    CHECK(n == 1 && strncmp(lines[0].text, "Not sent: ", 10) == 0);

    // The pipe gives up before sending (no path): failed with that text
    CHECK(mt_request(&t, MT_TRACE, PUB, 9, 0, &why) == 4);
    CHECK(mt_take(&t, &kind, prefix, &tag, 0));
    mt_fail(&t, "no path known: Discover path first");
    CHECK(t.state == MT_FAILED && strcmp(t.why, "no path known: Discover path first") == 0);

    // Queued but the mesh task never takes it (stopped): fails after the cap, millis wrap included
    CHECK(mt_request(&t, MT_TRACE, PUB, 10, 0xFFFFF000u, &why) == 5);
    mt_poll(&t, 0xFFFFF000u + MT_CAP_MS, NULL);
    CHECK(t.state == MT_FAILED);

    // Discovery: matched by the contact, the tag comes from MyMesh's RESP_SENT
    CHECK(mt_request(&t, MT_DISCOVER, PUB, 0, 100, &why) == 6);
    CHECK(mt_take(&t, &kind, prefix, &tag, 100) && kind == MT_DISCOVER && memcmp(prefix, PUB, 6) == 0);
    s       = sent(0x5555, 8000);
    s.flood = 1;
    mt_on_reply(&t, &s, 110);
    CHECK(t.state == MT_SENT && t.tag == 0x5555 && t.flood == 1);
    mt_on_tx(&t, 0x5555, 120); // only traces count on-air time
    CHECK(t.on_air_ms == 0);
    mf_discovery_t d;
    memset(&d, 0, sizeof(d));
    memcpy(d.prefix, PUB, 6);
    d.prefix[5] ^= 1;
    CHECK(!mt_on_discovery(&t, &d, 900)); // another contact
    d.prefix[5] ^= 1;
    d.out_len     = 0x42;
    d.out_path[0] = 0xa1;
    d.out_path[1] = 0xa2;
    d.out_path[2] = 0xb1;
    d.out_path[3] = 0xb2;
    d.in_len      = 1;
    d.in_path[0]  = 0xc3;
    CHECK(mt_on_discovery(&t, &d, 1110) && t.state == MT_DONE && mt_round_trip_ms(&t) == 1000);
    n = mt_format(&t, lines, MT_LINES_MAX);
    CHECK(n == 4);
    CHECK(strcmp(lines[0].text, "Path to it: 2 hops via a1a2,b1b2") == 0);
    CHECK(strcmp(lines[1].text, "Its answer: 1 hop via c3") == 0);
    CHECK(strcmp(lines[2].text, "Round trip: 1000 ms") == 0);

    // seq never 0
    t.seq = UINT32_MAX;
    t.state = MT_DONE;
    CHECK(mt_request(&t, MT_TRACE, PUB, 1, 0, &why) == 1);
}

static void snr_text(void) {
    char b[12];
    mt_format_snr(0, b, sizeof(b));
    CHECK(strcmp(b, "0") == 0);
    mt_format_snr(-1, b, sizeof(b));
    CHECK(strcmp(b, "-0.25") == 0);
    mt_format_snr(-128, b, sizeof(b));
    CHECK(strcmp(b, "-32") == 0);
    mt_format_snr(127, b, sizeof(b));
    CHECK(strcmp(b, "31.75") == 0);
}

int main(void) {
    frames();
    plans();
    states();
    snr_text();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_trace_test: all checks passed\n");
    return 0;
}
