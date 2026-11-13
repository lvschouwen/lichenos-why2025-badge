// Host test: repeater status history (#168): the schedule that transmits, the ring, the graphs' values and the setting
#include "mesh_rhist.h"

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

static rh_t t;

// A fresh table whose radio has been ready since 0 (#412): the schedule tests below start settled
static void fresh(void) {
    rh_init(&t);
    rh_radio(&t, true, 0);
}

static void key(uint8_t out[RH_PREFIX], uint8_t n) {
    memset(out, n, RH_PREFIX);
}

// The request as mesh_remote_head would copy it
static rr_head_t head(uint32_t seq, rr_state_t state, uint8_t const prefix[RH_PREFIX], uint16_t batt, uint32_t recv,
                      uint32_t up) {
    rr_head_t h;
    memset(&h, 0, sizeof(h));
    h.seq   = seq;
    h.op    = RR_STATUS;
    h.state = state;
    memcpy(h.prefix, prefix, RH_PREFIX);
    if (state == RR_DONE) {
        h.have_status = true;
        memcpy(h.status_prefix, prefix, RH_PREFIX);
        h.status.batt_mv     = batt;
        h.status.n_recv      = recv;
        h.status.n_sent      = recv / 2;
        h.status.up_secs     = up;
        h.status.noise_floor = -114;
    }
    if (state == RR_TIMEOUT) {
        h.why = "no answer (log in first?)";
    }
    return h;
}

// Nothing is ever due on a fresh table, whatever the time: off by default
static void off_by_default(void) {
    fresh();
    for (uint32_t now = 0; now < 100000; now += 37) {
        CHECK(rh_due(&t, now, false, UINT32_MAX, false) == -1);
    }
    uint8_t p[1][RH_PREFIX];
    uint16_t m[1];
    rh_load(&t, p, m, 0, 10, false);
    CHECK(rh_on_count(&t) == 0 && rh_due(&t, 5000, false, UINT32_MAX, false) == -1);
}

static void switched_on_and_interval(void) {
    uint8_t a[RH_PREFIX];
    key(a, 1);
    fresh();
    CHECK(rh_set(&t, a, true, 5, 1000) == RH_BAD); // under the 10 min floor
    CHECK(rh_set(&t, a, true, 11, 1000) == RH_BAD); // not offered
    CHECK(rh_on_count(&t) == 0);
    CHECK(rh_set(&t, a, true, 10, 1000) == RH_OK);
    CHECK(rh_due(&t, 1000 + RH_SOON_S - 1, false, UINT32_MAX, false) == -1);
    int idx = rh_due(&t, 1000 + RH_SOON_S, false, UINT32_MAX, false);
    CHECK(idx == 0);
    rh_asked(&t, idx, 7, 1020);
    CHECK(rh_due(&t, 1021, false, UINT32_MAX, false) == -1); // ours runs
    rr_head_t h = head(7, RR_SENT, a, 0, 0, 0);
    CHECK(!rh_observe(&t, &h, 1025));
    h = head(7, RR_DONE, a, 4120, 100, 3600);
    CHECK(rh_observe(&t, &h, 1030));
    CHECK(t.slot[0].count == 1 && t.slot[0].answers == 1 && t.slot[0].asks == 1 && t.asking == -1);
    CHECK(!rh_observe(&t, &h, 1031)); // the same answer is taken once
    CHECK(t.slot[0].count == 1);
    // the next ask: 10 min after the last, never sooner
    CHECK(rh_due(&t, 1020 + 599, false, UINT32_MAX, false) == -1);
    CHECK(rh_due(&t, 1020 + 600, false, UINT32_MAX, false) == 0);
    // off and on again right away does not ask before the interval is over
    CHECK(rh_set(&t, a, false, 10, 1100) == RH_OK);
    CHECK(rh_due(&t, 1020 + 600, false, UINT32_MAX, false) == -1);
    CHECK(rh_set(&t, a, true, 10, 1101) == RH_OK);
    CHECK(rh_due(&t, 1101 + RH_SOON_S, false, UINT32_MAX, false) == -1);
    CHECK(rh_due(&t, 1020 + 600, false, UINT32_MAX, false) == 0);
    CHECK(t.slot[0].count == 1); // the history survived off and on
    // a longer interval counts from the last ask
    CHECK(rh_set(&t, a, true, 60, 1200) == RH_OK);
    CHECK(t.slot[0].next_s == 1020 + 3600);
}

static void cap_on_slots(void) {
    uint8_t k[RH_SLOTS + 1][RH_PREFIX];
    fresh();
    for (int i = 0; i <= RH_SLOTS; i++) {
        key(k[i], (uint8_t)(10 + i));
    }
    for (int i = 0; i < RH_SLOTS; i++) {
        CHECK(rh_set(&t, k[i], true, 30, 0) == RH_OK);
    }
    CHECK(rh_set(&t, k[RH_SLOTS], true, 30, 0) == RH_FULL);
    CHECK(rh_on_count(&t) == RH_SLOTS && rh_find(&t, k[RH_SLOTS]) == -1);
    rh_set(&t, k[1], false, 30, 0); // one off: its slot is taken, its history goes
    CHECK(rh_set(&t, k[RH_SLOTS], true, 30, 0) == RH_OK);
    CHECK(rh_find(&t, k[RH_SLOTS]) == 1 && rh_find(&t, k[1]) == -1 && rh_on_count(&t) == RH_SLOTS);
}

// At a start: no burst, the first after RH_FIRST_S, the others RH_STAGGER_S apart, and every ask RH_GAP_S apart
static void start_is_staggered(void) {
    uint8_t  k[3][RH_PREFIX];
    uint16_t m[3] = {10, 10, 10};
    for (int i = 0; i < 3; i++) {
        key(k[i], (uint8_t)(20 + i));
    }
    fresh();
    rh_load(&t, k, m, 3, 30, false);
    CHECK(rh_on_count(&t) == 3);
    CHECK(rh_due(&t, 30 + RH_FIRST_S - 1, false, UINT32_MAX, false) == -1);
    CHECK(rh_due(&t, 30 + RH_FIRST_S, false, UINT32_MAX, false) == 0);
    CHECK(t.slot[1].next_s == 30 + RH_FIRST_S + RH_STAGGER_S && t.slot[2].next_s == 30 + RH_FIRST_S + 2 * RH_STAGGER_S);
    // all overdue at once: still one at a time, RH_GAP_S apart
    uint32_t now = 30 + RH_FIRST_S + 2 * RH_STAGGER_S;
    int      idx = rh_due(&t, now, false, UINT32_MAX, false);
    CHECK(idx == 0);
    rh_asked(&t, idx, 1, now);
    rr_head_t h = head(1, RR_DONE, k[0], 4000, 10, 100);
    rh_observe(&t, &h, now + 2);
    CHECK(rh_due(&t, now + RH_GAP_S - 1, false, UINT32_MAX, false) == -1);
    CHECK(rh_due(&t, now + RH_GAP_S, false, UINT32_MAX, false) == 1);
}

static void busy_and_quiet(void) {
    uint8_t a[RH_PREFIX];
    key(a, 3);
    fresh();
    rh_set(&t, a, true, 10, 0);
    CHECK(rh_due(&t, 100, true, UINT32_MAX, false) == -1);             // another request runs
    CHECK(rh_due(&t, 100, false, RH_QUIET_S - 1, false) == -1);        // the person asked a moment ago
    CHECK(rh_due(&t, 100, false, RH_QUIET_S, false) == 0);
    rr_head_t h = head(5, RR_DONE, a, 0, 0, 0);
    h.age_ms    = 30000;
    CHECK(rh_other_age_s(&t, &h) == 30);
    rh_asked(&t, 0, 5, 100);
    CHECK(rh_other_age_s(&t, &h) == UINT32_MAX); // our own request is not the person's
    CHECK(rh_other_age_s(&t, NULL) == UINT32_MAX);
    // could not start: retried in RH_RETRY_S, not a miss
    fresh();
    rh_set(&t, a, true, 10, 0);
    rh_not_started(&t, 0, "LoRa is off", 50);
    CHECK(t.slot[0].misses == 0 && t.slot[0].next_s == 50 + RH_RETRY_S && strcmp(t.slot[0].why, "LoRa is off") == 0);
}

static void backoff(void) {
    uint8_t a[RH_PREFIX];
    key(a, 4);
    fresh();
    rh_set(&t, a, true, 10, 0);
    uint32_t now = RH_SOON_S, seq = 1;
    uint32_t want[] = {1200, 2400, 4800, 4800, 4800};
    for (int i = 0; i < 5; i++) {
        CHECK(rh_due(&t, now, false, UINT32_MAX, false) == 0);
        rh_asked(&t, 0, seq, now);
        rr_head_t h = head(seq, RR_TIMEOUT, a, 0, 0, 0);
        CHECK(rh_observe(&t, &h, now + 10));
        CHECK(t.slot[0].misses == i + 1);
        CHECK(t.slot[0].next_s - now == want[i]);
        CHECK(rh_due(&t, t.slot[0].next_s - 1, false, UINT32_MAX, false) == -1);
        now = t.slot[0].next_s;
        seq++;
    }
    CHECK(strcmp(t.slot[0].why, "no answer (log in first?)") == 0);
    rh_asked(&t, 0, seq, now);
    rr_head_t h = head(seq, RR_DONE, a, 4000, 1, 1);
    rh_observe(&t, &h, now + 3);
    CHECK(t.slot[0].misses == 0 && t.slot[0].why == NULL && t.slot[0].next_s == now + 600);
    // our end not seen (the person's request replaced it): no miss, the normal wait
    rh_asked(&t, 0, seq + 1, now + 600);
    h = head(seq + 5, RR_SENT, a, 0, 0, 0);
    CHECK(rh_observe(&t, &h, now + 700));
    CHECK(t.asking == -1 && t.slot[0].misses == 0 && t.slot[0].next_s == now + 1200);
    // MeshCore gone: the same
    rh_asked(&t, 0, seq + 6, now + 1200);
    CHECK(rh_observe(&t, NULL, now + 1201) && t.asking == -1 && t.slot[0].misses == 0);
}

// A status the person asked for lands in the ring of a switched-on repeater and saves an ask; not for one that is off
static void persons_answer(void) {
    uint8_t a[RH_PREFIX], b[RH_PREFIX];
    key(a, 5);
    key(b, 6);
    fresh();
    rh_set(&t, a, true, 30, 0);
    rr_head_t h = head(40, RR_DONE, a, 3900, 50, 7200);
    CHECK(rh_observe(&t, &h, 15));
    CHECK(t.slot[0].count == 1 && t.slot[0].next_s == 15 + 1800);
    CHECK(!rh_observe(&t, &h, 16));
    h = head(41, RR_DONE, b, 3900, 50, 7200);
    CHECK(!rh_observe(&t, &h, 17) && rh_find(&t, b) == -1);
    rh_set(&t, a, false, 30, 20);
    h = head(42, RR_DONE, a, 3900, 50, 7200);
    CHECK(!rh_observe(&t, &h, 21) && t.slot[0].count == 1);
    rh_forget(&t, a);
    CHECK(rh_find(&t, a) == -1);
}

static void ring_and_series(void) {
    uint8_t a[RH_PREFIX];
    key(a, 7);
    fresh();
    rh_set(&t, a, true, 10, 0);
    for (uint32_t i = 0; i < RH_SAMPLES + 5; i++) {
        rr_head_t h = head(100 + i, RR_DONE, a, (uint16_t)(4000 + i), 1000 + 10 * i, 3600 * (i + 1));
        rh_observe(&t, &h, 600 * (i + 1));
    }
    static rh_sample_t s[RH_SAMPLES];
    size_t             n = rh_samples(&t.slot[0], s, RH_SAMPLES);
    CHECK(n == RH_SAMPLES && t.slot[0].count == RH_SAMPLES);
    CHECK(s[0].batt_mv == 4005 && s[n - 1].batt_mv == 4000 + RH_SAMPLES + 4); // the oldest went
    size_t m = rh_samples(&t.slot[0], s, 3);
    CHECK(m == 3 && s[2].batt_mv == 4000 + RH_SAMPLES + 4 && s[0].batt_mv == 4000 + RH_SAMPLES + 2);

    // rates: 10 packets in 10 min = 60/h; a restart between two samples leaves a gap; a missing battery too
    rh_sample_t x[4] = {
        {.at_s = 0, .up_secs = 100, .n_recv = 10, .n_sent = 4, .batt_mv = 4100, .noise = -110},
        {.at_s = 600, .up_secs = 700, .n_recv = 20, .n_sent = 6, .batt_mv = 0, .noise = -112},
        {.at_s = 1200, .up_secs = 50, .n_recv = 2, .n_sent = 1, .batt_mv = 4090, .noise = -111},
        {.at_s = 2400, .up_secs = 1250, .n_recv = 32, .n_sent = 1, .batt_mv = 4080, .noise = -115},
    };
    int32_t v[4];
    rh_series(x, 4, RH_RECV_RATE, v);
    CHECK(v[0] == RH_NONE && v[1] == 60 && v[2] == RH_NONE && v[3] == 90);
    rh_series(x, 4, RH_SENT_RATE, v);
    CHECK(v[1] == 12 && v[3] == 0);
    rh_series(x, 4, RH_BATTERY, v);
    CHECK(v[0] == 4100 && v[1] == RH_NONE && v[3] == 4080);
    int32_t lo, hi;
    CHECK(rh_range(v, 4, RH_BATTERY, &lo, &hi) && lo == 4040 && hi == 4140); // widened to 0.1 V
    rh_series(x, 4, RH_NOISE, v);
    CHECK(rh_range(v, 4, RH_NOISE, &lo, &hi) && lo == -117 && hi == -107);
    rh_series(x, 4, RH_UPTIME, v);
    CHECK(v[0] == 0 && rh_range(v, 4, RH_UPTIME, &lo, &hi) && lo == 0 && hi == 10);
    int32_t none[2] = {RH_NONE, RH_NONE};
    CHECK(!rh_range(none, 2, RH_RECV_RATE, &lo, &hi));
    int32_t big[2] = {5, 250};
    CHECK(rh_range(big, 2, RH_RECV_RATE, &lo, &hi) && lo == 0 && hi == 250);
}

static void texts(void) {
    char out[32];
    rh_interval_text(10, out, sizeof(out));
    CHECK_STR(out, "10 min");
    rh_interval_text(180, out, sizeof(out));
    CHECK_STR(out, "3 h");
    CHECK(rh_interval_next(10) == 30 && rh_interval_next(30) == 60 && rh_interval_next(60) == 180 &&
          rh_interval_next(180) == 10);
    uint8_t a[RH_PREFIX];
    key(a, 8);
    fresh();
    rh_next_text(NULL, false, 0, out, sizeof(out));
    CHECK_STR(out, "Off");
    rh_set(&t, a, true, 10, 0);
    rh_next_text(&t.slot[0], t.asking == 0, 0, out, sizeof(out));
    CHECK_STR(out, "in 1 min");
    rh_next_text(&t.slot[0], t.asking == 0, RH_SOON_S, out, sizeof(out));
    CHECK_STR(out, "now");
    t.slot[0].next_s = 4800 + 30;
    rh_next_text(&t.slot[0], t.asking == 0, 30, out, sizeof(out));
    CHECK_STR(out, "in 1 h 20 min");
    rh_asked(&t, 0, 3, 4830);
    rh_next_text(&t.slot[0], t.asking == 0, 4831, out, sizeof(out));
    CHECK_STR(out, "Asking...");
    rh_value_text(RH_BATTERY, 4125, out, sizeof(out));
    CHECK_STR(out, "4.12 V");
    rh_value_text(RH_NOISE, -114, out, sizeof(out));
    CHECK_STR(out, "-114 dBm");
    rh_value_text(RH_RECV_RATE, 120, out, sizeof(out));
    CHECK_STR(out, "120/h");
    rh_value_text(RH_UPTIME, RH_NONE, out, sizeof(out));
    CHECK_STR(out, "-");
}

// The console scope changed (#290): the repeaters a console line switched on go off, the screen's stay on
static void console_off(void) {
    uint8_t a[RH_PREFIX], b[RH_PREFIX], c[RH_PREFIX];
    key(a, 1), key(b, 2), key(c, 3);
    fresh();
    rh_set(&t, a, true, 30, 0);
    rh_set(&t, b, true, 60, 0);
    rh_set(&t, c, true, 10, 0);
    rh_set_console(&t, a, true);
    rh_set_console(&t, c, true);
    rh_set(&t, c, false, 10, 0); // already off: not counted
    CHECK(rh_off_console(&t) == 1);
    CHECK(!t.slot[rh_find(&t, a)].on && t.slot[rh_find(&t, b)].on && rh_on_count(&t) == 1);
    CHECK(rh_off_console(&t) == 0);
    char out[RH_CFG_MAX];
    rh_cfg_encode(&t, out, sizeof(out));
    CHECK_STR(out, "020202020202:60");
}

static void setting(void) {
    uint8_t a[RH_PREFIX] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6}, b[RH_PREFIX] = {0, 1, 2, 3, 4, 5}, c[RH_PREFIX];
    key(c, 9);
    fresh();
    char out[RH_CFG_MAX];
    rh_cfg_encode(&t, out, sizeof(out));
    CHECK_STR(out, "");
    rh_set(&t, a, true, 30, 0);
    rh_set(&t, b, true, 180, 0);
    rh_set(&t, c, true, 10, 0);
    rh_set(&t, c, false, 10, 0); // off is not saved
    uint8_t k_none[RH_PREFIX];
    key(k_none, 0x77);
    rh_cfg_encode(&t, out, sizeof(out));
    CHECK_STR(out, "a1b2c3d4e5f6:30,000102030405:180");
    uint8_t  p[RH_SLOTS][RH_PREFIX];
    uint16_t m[RH_SLOTS];
    CHECK(rh_cfg_decode(out, p, m, NULL, RH_SLOTS) == 2);
    CHECK(memcmp(p[0], a, RH_PREFIX) == 0 && m[0] == 30 && memcmp(p[1], b, RH_PREFIX) == 0 && m[1] == 180);
    // bad entries are skipped: not hex, a short key, an interval not offered (5 min), a duplicate, junk
    CHECK(rh_cfg_decode("zz0102030405:30,0102:30,010203040506:5,A1B2C3D4E5F6:10,a1b2c3d4e5f6:30,x", p, m, NULL, RH_SLOTS) == 1);
    CHECK(memcmp(p[0], a, RH_PREFIX) == 0 && m[0] == 10);
    CHECK(rh_cfg_decode("", p, m, NULL, RH_SLOTS) == 0 && rh_cfg_decode(NULL, p, m, NULL, RH_SLOTS) == 0);
    CHECK(rh_cfg_decode("010101010101:10,020202020202:10,030303030303:10,040404040404:10", p, m, NULL, RH_SLOTS) == RH_SLOTS);
    char small[20];
    rh_cfg_encode(&t, small, sizeof(small)); // the second entry does not fit: left out whole
    CHECK_STR(small, "a1b2c3d4e5f6:30");
    // #290: a console line's switch-on is saved with a 'c' and read back with it; old entries have none
    bool con[RH_SLOTS];
    memset(con, 1, sizeof(con));
    rh_set_console(&t, b, true);
    rh_set_console(&t, k_none, true); // no slot: nothing changes
    rh_cfg_encode(&t, out, sizeof(out));
    CHECK_STR(out, "a1b2c3d4e5f6:30,000102030405:180c");
    CHECK(rh_cfg_decode(out, p, m, con, RH_SLOTS) == 2 && !con[0] && con[1] && m[1] == 180);
    CHECK(rh_cfg_decode("010101010101:10c,020202020202:cc,030303030303:c,040404040404:30", p, m, con, RH_SLOTS) == 2);
    CHECK(con[0] && m[0] == 10 && !con[1] && m[1] == 30);
    rh_set_console(&t, b, false);
    rh_cfg_encode(&t, out, sizeof(out));
    CHECK_STR(out, "a1b2c3d4e5f6:30,000102030405:180");
    // the longest setting still fits: every slot console, 180 min
    fresh();
    for (int i = 0; i < RH_SLOTS; i++) {
        uint8_t k[RH_PREFIX];
        key(k, 0x40 + i);
        rh_set(&t, k, true, 180, 0);
        rh_set_console(&t, k, true);
    }
    rh_cfg_encode(&t, out, sizeof(out));
    CHECK(rh_cfg_decode(out, p, m, con, RH_SLOTS) == RH_SLOTS && con[RH_SLOTS - 1]);
}

// A screen that asks a repeater holds the poll back (review #168): nothing is due while it is open
static void held_by_screen(void) {
    uint8_t a[RH_PREFIX];
    key(a, 30);
    fresh();
    rh_set(&t, a, true, 10, 0);
    CHECK(rh_due(&t, 5000, false, UINT32_MAX, true) == -1);
    CHECK(rh_due(&t, 5000, false, UINT32_MAX, false) == 0);
}

// After a crash, a watchdog or a brownout every repeater waits its whole interval: a restart loop cannot ask under it
static void crash_boot_waits_interval(void) {
    uint8_t  k[2][RH_PREFIX];
    uint16_t m[2] = {30, 10};
    key(k[0], 31);
    key(k[1], 32);
    fresh();
    rh_load(&t, k, m, 2, 40, true);
    CHECK(t.slot[0].next_s == 40 + 1800 && t.slot[1].next_s == 40 + 600 + RH_STAGGER_S);
    CHECK(rh_due(&t, 40 + 600 + RH_STAGGER_S - 1, false, UINT32_MAX, false) == -1);
    fresh();
    rh_load(&t, k, m, 2, 40, false); // a clean start: the short wait
    CHECK(t.slot[0].next_s == 40 + RH_FIRST_S && t.slot[1].next_s == 40 + RH_FIRST_S + RH_STAGGER_S);
}

// What raises the on-air rate asks first; off and a longer interval never do
static void confirm_policy(void) {
    uint8_t a[RH_PREFIX];
    key(a, 33);
    fresh();
    CHECK(rh_needs_confirm(&t, a, true, 30));  // on
    CHECK(!rh_needs_confirm(&t, a, false, 30)); // off
    rh_set(&t, a, true, 30, 0);
    CHECK(!rh_needs_confirm(&t, a, true, 60)); // longer
    CHECK(!rh_needs_confirm(&t, a, true, 30)); // the same
    CHECK(rh_needs_confirm(&t, a, true, 10));  // shorter
    rh_set(&t, a, false, 30, 0);
    CHECK(rh_needs_confirm(&t, a, true, 60)); // on again, even with a longer interval
}

// A repeater whose slot went to another keeps its last ask: switched on again it waits the rest of its interval
static void last_ask_survives_eviction(void) {
    uint8_t a[RH_PREFIX], o[3][RH_PREFIX];
    key(a, 40);
    for (int i = 0; i < 3; i++) {
        key(o[i], (uint8_t)(41 + i));
    }
    fresh();
    rh_set(&t, a, true, 30, 0);
    CHECK(rh_due(&t, RH_SOON_S, false, UINT32_MAX, false) == 0);
    rh_asked(&t, 0, 1, 100);
    rr_head_t h = head(1, RR_DONE, a, 4000, 1, 1);
    rh_observe(&t, &h, 105);
    rh_set(&t, a, false, 30, 110);
    for (int i = 0; i < 3; i++) {
        CHECK(rh_set(&t, o[i], true, 30, 120) == RH_OK); // the third takes a's slot
    }
    CHECK(rh_find(&t, a) == -1);
    rh_set(&t, o[0], false, 30, 130);
    CHECK(rh_set(&t, a, true, 30, 140) == RH_OK);
    int idx = rh_find(&t, a);
    CHECK(idx >= 0 && t.slot[idx].next_s == 100 + 1800);
    // a full recent table drops its oldest entry, never the newest
    fresh();
    uint8_t p[RH_RECENT + 1][RH_PREFIX];
    for (int i = 0; i <= RH_RECENT; i++) {
        key(p[i], (uint8_t)(100 + i));
        CHECK(rh_set(&t, p[i], true, 30, 0) == RH_OK);
        rh_asked(&t, rh_find(&t, p[i]), (uint32_t)i + 1, 1000 + (uint32_t)i);
        rr_head_t hh = head((uint32_t)i + 1, RR_TIMEOUT, p[i], 0, 0, 0);
        rh_observe(&t, &hh, 1001 + (uint32_t)i);
        rh_set(&t, p[i], false, 30, 1001 + (uint32_t)i);
    }
    bool first = false, last = false;
    for (int i = 0; i < RH_RECENT; i++) {
        first = first || memcmp(t.recent[i].prefix, p[0], RH_PREFIX) == 0;
        last  = last || (memcmp(t.recent[i].prefix, p[RH_RECENT], RH_PREFIX) == 0 && t.recent[i].ask_s == 1000 + RH_RECENT);
    }
    CHECK(!first && last);
}

// #412: the poll waits for the radio's settings. At a start MeshCore's settings are pending until the LoRa task sends
// them; a poll before would be cut short by them (C6 code 3, a failed send). Not ready: nobody is asked; ready: asked
// RH_SETTLE_S after the first tick that saw it ready; a tick gap (a stop) makes a stale "ready" count from new.
static void waits_for_radio(void) {
    uint8_t a[RH_PREFIX];
    key(a, 50);
    rh_init(&t);
    CHECK(!rh_radio_settled(&t, 1000)); // a fresh table: not ready until a tick says so
    rh_set(&t, a, true, 10, 0);
    CHECK(rh_due(&t, 1000, false, UINT32_MAX, false) == -1);
    for (uint32_t now = 1000; now < 1010; now++) { // the settings pending: wait, whatever the schedule says
        rh_radio(&t, false, now);
        CHECK(rh_due(&t, now, false, UINT32_MAX, false) == -1);
    }
    rh_radio(&t, true, 1010);
    CHECK(rh_due(&t, 1010, false, UINT32_MAX, false) == -1);
    rh_radio(&t, true, 1011);
    CHECK(rh_due(&t, 1011, false, UINT32_MAX, false) == -1);
    rh_radio(&t, true, 1012);
    CHECK(rh_due(&t, 1012, false, UINT32_MAX, false) == 0); // RH_SETTLE_S after the first ready tick
    // MeshCore stopped and started again: no tick for a while, the first tick after sees "ready" (a stale state)
    rh_asked(&t, 0, 1, 1012);
    rr_head_t h = head(1, RR_DONE, a, 4000, 1, 1);
    rh_observe(&t, &h, 1015);
    uint32_t start = 1012 + 600 + 300; // the poll is overdue by then: before #412 it went out at once
    rh_radio(&t, true, start);
    CHECK(rh_due(&t, start, false, UINT32_MAX, false) == -1);
    rh_radio(&t, true, start + 1);
    CHECK(rh_due(&t, start + 1, false, UINT32_MAX, false) == -1);
    rh_radio(&t, true, start + 2);
    CHECK(rh_due(&t, start + 2, false, UINT32_MAX, false) == 0);
    // ready, then pending again (a radio change) before the ask: waits once more
    rh_radio(&t, false, start + 3);
    CHECK(rh_due(&t, start + 3, false, UINT32_MAX, false) == -1);
}

// #412: a poll refused because the radio was not ready is no ask and no miss; it is tried again after RH_DEFER_S,
// doubling, never before its regular time. A refusal for another reason is as before (RH_RETRY_S, not a miss).
static void refused_not_ready(void) {
    uint8_t a[RH_PREFIX];
    key(a, 51);
    fresh();
    rh_set(&t, a, true, 10, 0);
    uint32_t now = RH_SOON_S;
    uint32_t want[] = {RH_DEFER_S, 2 * RH_DEFER_S, 4 * RH_DEFER_S, 8 * RH_DEFER_S, 8 * RH_DEFER_S};
    for (int i = 0; i < 5; i++) {
        rh_radio(&t, true, now - 3);
        rh_radio(&t, true, now - 2);
        rh_radio(&t, true, now);
        CHECK(rh_due(&t, now, false, UINT32_MAX, false) == 0);
        rh_not_ready(&t, 0, now);
        CHECK(t.slot[0].misses == 0 && t.slot[0].asks == 0 && t.slot[0].ask_s == 0 && t.last_ask_s == 0);
        CHECK(t.slot[0].next_s - now == want[i]);
        CHECK(!rh_radio_settled(&t, now + 100)); // counts as not ready until a tick sees it ready again
        now = t.slot[0].next_s;
    }
    CHECK(strcmp(t.slot[0].why, "the radio was not ready") == 0);
    rh_radio(&t, true, now - 2);
    rh_radio(&t, true, now);
    CHECK(rh_due(&t, now, false, UINT32_MAX, false) == 0);
    rh_asked(&t, 0, 7, now);
    CHECK(t.slot[0].defers == 0 && t.slot[0].next_s == now + 600); // the regular interval, from the real ask
    // a deferral never moves a later time sooner
    t.slot[0].next_s = now + 5000;
    rh_not_ready(&t, 0, now + 10);
    CHECK(t.slot[0].next_s == now + 5000);
    // refused for another reason: as before
    fresh();
    rh_set(&t, a, true, 10, 0);
    rh_not_started(&t, 0, "LoRa is off", 50);
    CHECK(t.slot[0].misses == 0 && t.slot[0].defers == 0 && t.slot[0].next_s == 50 + RH_RETRY_S);
}

// #412: no retry storm. A day on a fake clock, one tick a second, the radio flapping between ready and not and every
// other attempt refused as not ready: asks per repeater never come closer than its interval, refused attempts never
// closer than RH_DEFER_S, and there are never more asks than with a radio that is always ready.
static uint32_t lcg(uint32_t *x) {
    *x = *x * 1103515245u + 12345u;
    return *x >> 16;
}

static unsigned simulate(bool flaky, uint32_t *min_ask_gap, uint32_t *min_refused_gap) {
    uint8_t a[RH_PREFIX], b[RH_PREFIX];
    key(a, 52);
    key(b, 53);
    fresh();
    rh_set(&t, a, true, 10, 0);
    rh_set(&t, b, true, 30, 0);
    uint32_t seed = 412, seq = 0, last_ask[RH_SLOTS] = {0}, last_refused[RH_SLOTS] = {0};
    unsigned asks = 0;
    *min_ask_gap = *min_refused_gap = UINT32_MAX;
    for (uint32_t now = 1; now < 86400; now++) {
        bool ready = !flaky || lcg(&seed) % 4 != 0;
        rh_radio(&t, ready, now);
        if (t.asking >= 0 && now - t.slot[t.asking].ask_s >= 15) {
            rr_head_t h = head(seq, RR_DONE, t.slot[t.asking].prefix, 4000, now, now);
            rh_observe(&t, &h, now);
        }
        int idx = rh_due(&t, now, false, UINT32_MAX, false);
        if (idx < 0) {
            continue;
        }
        if (flaky && lcg(&seed) % 2) {
            if (last_refused[idx] && now - last_refused[idx] < *min_refused_gap) {
                *min_refused_gap = now - last_refused[idx];
            }
            last_refused[idx] = now;
            rh_not_ready(&t, idx, now);
            continue;
        }
        if (last_ask[idx] && now - last_ask[idx] < *min_ask_gap) {
            *min_ask_gap = now - last_ask[idx];
        }
        last_ask[idx] = now;
        rh_asked(&t, idx, ++seq, now);
        asks++;
    }
    return asks;
}

static void no_retry_storm(void) {
    uint32_t gap, refused_gap;
    unsigned steady = simulate(false, &gap, &refused_gap);
    CHECK(gap >= 600 && refused_gap == UINT32_MAX);
    unsigned flaky = simulate(true, &gap, &refused_gap);
    CHECK(gap >= 600);                 // the shortest interval: a refusal never shortens the regular timer
    CHECK(refused_gap >= RH_DEFER_S);  // at most one deferred attempt per bound
    CHECK(flaky > 0 && flaky <= steady);
    CHECK(steady <= 86400 / 600 + 86400 / 1800 + 2);
}

int main(void) {
    off_by_default();
    switched_on_and_interval();
    cap_on_slots();
    start_is_staggered();
    busy_and_quiet();
    backoff();
    persons_answer();
    ring_and_series();
    texts();
    setting();
    console_off();
    held_by_screen();
    crash_boot_waits_interval();
    confirm_policy();
    last_ask_survives_eviction();
    waits_for_radio();
    refused_not_ready();
    no_retry_storm();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_rhist_test: all passed\n");
    return 0;
}
