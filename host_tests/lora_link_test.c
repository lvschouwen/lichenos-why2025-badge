// Host tests for the P4 LoRa link logic: config file, supervisor, status line
#include "lora_link.h"
#include "mutate.h"

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

static lora_config_t const defaults = LORA_CONFIG_DEFAULT;

static bool same(lora_config_t const *a, lora_config_t const *b) {
    return a->freq_hz == b->freq_hz && a->bw_hz == b->bw_hz && a->sf == b->sf && a->cr == b->cr &&
           a->sync_word == b->sync_word && a->preamble == b->preamble;
}

static bool parse(char const *text, lora_config_t *out, char *err) {
    return lora_cfg_parse(text, strlen(text), out, err, 128);
}

static void cfg_defaults_when_empty(void) {
    lora_config_t c;
    char          err[128] = "";
    CHECK(parse("", &c, err));
    CHECK(same(&c, &defaults));
}

static void cfg_skel_file_matches_defaults(void) {
    static char text[2048];
    FILE       *fp = fopen(SKEL_LORA_TOML, "r");
    CHECK(fp);
    if (!fp) {
        return;
    }
    size_t n = fread(text, 1, sizeof(text) - 1, fp);
    fclose(fp);
    text[n] = '\0';

    lora_config_t c;
    char          err[128] = "";
    CHECK(lora_cfg_parse(text, n, &c, err, sizeof(err)));
    CHECK(same(&c, &defaults));
}

static void cfg_overrides(void) {
    lora_config_t c;
    char          err[128] = "";
    CHECK(parse("frequency_mhz = 868.1\nbandwidth_khz = 125\nspreading_factor = 9\ncoding_rate = 8\n", &c, err));
    CHECK(c.freq_hz == 868100000);
    CHECK(c.bw_hz == 125000);
    CHECK(c.sf == 9);
    CHECK(c.cr == 8);
    CHECK(c.sync_word == 0x12);
    CHECK(c.preamble == 16);

    CHECK(parse("bandwidth_khz = 41.7\nsync_word = 0x34\npreamble = 8\nfrequency_mhz = 869\n", &c, err));
    CHECK(c.bw_hz == 41700);
    CHECK(c.sync_word == 0x34);
    CHECK(c.preamble == 8);
    CHECK(c.freq_hz == 869000000);

    CHECK(parse("bandwidth_khz = 7.8\n", &c, err));
    CHECK(c.bw_hz == 7800);
}

static void cfg_bad_values_fall_back(void) {
    char const *bad[] = {
        "spreading_factor = 13\n",
        "frequency_mhz = 433.0\n",
        "bandwidth_khz = 60\n",
        "spreading_factor = \"7\"\n",
        "not toml [\n",
        "sync_word = 256\n",
        "preamble = -1\n",
        "coding_rate = 4.5\n",
        "frequency_mhz = 5164.618\n",
        "bandwidth_khz = 4295029.796\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        lora_config_t c;
        char          err[128] = "";
        if (parse(bad[i], &c, err) || !same(&c, &defaults) || !err[0]) {
            printf("FAIL bad config %zu accepted or not reset: %s", i, bad[i]);
            failures++;
        }
    }
}

static void supervisor_starts_with_config(void) {
    lora_sup_t s;
    lora_sup_init(&s, 0);
    CHECK(lora_sup_tick(&s, 0) == (LORA_ACT_SEND_CONFIG | LORA_ACT_RX_START));
    CHECK(lora_sup_tick(&s, 1000) == 0);
    CHECK(lora_sup_tick(&s, 29999) == 0);
    CHECK(lora_sup_tick(&s, 30000) == LORA_ACT_STATUS_REQ);
    CHECK(lora_sup_tick(&s, 30001) == 0);
}

static void supervisor_reconfigures_after_c6_restart(void) {
    lora_sup_t s;
    lora_sup_init(&s, 0);
    lora_sup_tick(&s, 0);
    lora_sup_tick(&s, 30000);

    lora_status_t st = {.state = LORA_STATE_RX};
    CHECK(lora_sup_on_status(&s, &st, 30005) == 0);

    st.state = LORA_STATE_UNCONFIGURED;
    lora_sup_tick(&s, 60000);
    CHECK(lora_sup_on_status(&s, &st, 60005) == (LORA_ACT_SEND_CONFIG | LORA_ACT_RX_START));

    st.state = LORA_STATE_IDLE;
    CHECK(lora_sup_on_status(&s, &st, 60010) == (LORA_ACT_SEND_CONFIG | LORA_ACT_RX_START));

    // A radio that failed is reported, not retried in a loop
    st.state = LORA_STATE_ERROR;
    CHECK(lora_sup_on_status(&s, &st, 60015) == 0);
}

static void supervisor_restarts_after_reattach(void) {
    // #14: after an ESP-Hosted re-attach the LoRa task runs lora_sup_init again on its used supervisor, late in the
    // boot and near the uint32 wrap. The first tick must re-send config + RX start, and the silence count starts over.
    uint32_t const starts[] = {5000000u, 0xFFFFF000u};
    for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
        lora_sup_t s;
        lora_sup_init(&s, 0);
        lora_sup_tick(&s, 0);
        lora_sup_tick(&s, 30000);
        lora_sup_tick(&s, 60000); // two unanswered polls: one more would reconfigure
        uint32_t t = starts[i];
        lora_sup_init(&s, t);
        CHECK(lora_sup_tick(&s, t) == (LORA_ACT_SEND_CONFIG | LORA_ACT_RX_START));
        CHECK(lora_sup_tick(&s, t + 1000) == 0);
        CHECK(lora_sup_tick(&s, t + 30000) == LORA_ACT_STATUS_REQ);
        CHECK(lora_sup_tick(&s, t + 60000) == LORA_ACT_STATUS_REQ);
        CHECK(lora_sup_tick(&s, t + 90000) == (LORA_ACT_STATUS_REQ | LORA_ACT_SEND_CONFIG | LORA_ACT_RX_START));
    }
}

static void c6_link_across_a_reset(void) {
    // #14 T-B: lost, then back: the task tells the owner "down", re-inits, and tells "up" once the C6 speaks
    lora_c6_link_t l;
    lora_c6_link_init(&l);
    CHECK(!l.lost);
    CHECK(lora_c6_link_take(&l) == 0);
    CHECK(lora_c6_link_heard(&l) == 0); // normal traffic tells nobody anything
    lora_c6_link_back(&l);              // back without lost: nothing to do
    CHECK(lora_c6_link_take(&l) == 0);

    lora_c6_link_lost(&l);
    CHECK(l.lost);
    CHECK(lora_c6_link_take(&l) == LORA_C6_TELL_DOWN);
    CHECK(lora_c6_link_take(&l) == 0);
    CHECK(l.lost);                      // no polls while lost
    CHECK(lora_c6_link_heard(&l) == 0); // a late message from before the loss
    lora_c6_link_back(&l);
    CHECK(lora_c6_link_take(&l) == LORA_C6_REINIT);
    CHECK(!l.lost);
    CHECK(lora_c6_link_take(&l) == 0); // REINIT once
    CHECK(lora_c6_link_heard(&l) == LORA_C6_TELL_UP);
    CHECK(lora_c6_link_heard(&l) == 0);

    // lost and back before the task looked: both, down first
    lora_c6_link_lost(&l);
    lora_c6_link_back(&l);
    CHECK(lora_c6_link_take(&l) == (LORA_C6_TELL_DOWN | LORA_C6_REINIT));
    // lost again before the C6 spoke: no "up" for the old attach
    lora_c6_link_lost(&l);
    CHECK(lora_c6_link_heard(&l) == 0);
    CHECK(lora_c6_link_take(&l) == LORA_C6_TELL_DOWN);
    // a second lost while lost: one more "down" is harmless, still one REINIT after back
    lora_c6_link_lost(&l);
    lora_c6_link_back(&l);
    lora_c6_link_back(&l);
    CHECK(lora_c6_link_take(&l) == (LORA_C6_TELL_DOWN | LORA_C6_REINIT));
    CHECK(lora_c6_link_take(&l) == 0);
}

static void supervisor_retries_failed_radio_slowly(void) {
    lora_sup_t s;
    lora_sup_init(&s, 0);
    lora_sup_tick(&s, 0);

    // Every third ERROR report (about every 90 s) tries the radio again, so one SPI hiccup does not end reception
    lora_status_t st = {.state = LORA_STATE_ERROR};
    CHECK(lora_sup_on_status(&s, &st, 30000) == 0);
    CHECK(lora_sup_on_status(&s, &st, 60000) == 0);
    CHECK(lora_sup_on_status(&s, &st, 90000) == (LORA_ACT_SEND_CONFIG | LORA_ACT_RX_START));
    CHECK(lora_sup_on_status(&s, &st, 120000) == 0);

    // Receiving again resets the count
    st.state = LORA_STATE_RX;
    lora_sup_on_status(&s, &st, 150000);
    st.state = LORA_STATE_ERROR;
    CHECK(lora_sup_on_status(&s, &st, 180000) == 0);
    CHECK(lora_sup_on_status(&s, &st, 210000) == 0);
    CHECK(lora_sup_on_status(&s, &st, 240000) == (LORA_ACT_SEND_CONFIG | LORA_ACT_RX_START));
}

static void supervisor_reconfigures_after_silence(void) {
    lora_sup_t s;
    lora_sup_init(&s, 0);
    lora_sup_tick(&s, 0);
    CHECK(lora_sup_tick(&s, 30000) == LORA_ACT_STATUS_REQ);
    CHECK(lora_sup_tick(&s, 60000) == LORA_ACT_STATUS_REQ);
    CHECK(lora_sup_tick(&s, 90000) == (LORA_ACT_SEND_CONFIG | LORA_ACT_RX_START | LORA_ACT_STATUS_REQ));

    // An answer resets the count
    CHECK(lora_sup_tick(&s, 120000) == LORA_ACT_STATUS_REQ);
    lora_status_t st = {.state = LORA_STATE_RX};
    lora_sup_on_status(&s, &st, 120010);
    CHECK(lora_sup_tick(&s, 150000) == LORA_ACT_STATUS_REQ);
    CHECK(lora_sup_tick(&s, 180000) == LORA_ACT_STATUS_REQ);

    // Survives the millisecond counter wrapping
    lora_sup_init(&s, 0xFFFFF000u);
    lora_sup_tick(&s, 0xFFFFF000u);
    CHECK(lora_sup_tick(&s, 0xFFFFF000u + LORA_POLL_MS) == LORA_ACT_STATUS_REQ);
}

static void status_line_format(void) {
    lora_status_t st = {
        .state         = LORA_STATE_RX,
        .rssi_x10      = -1052,
        .packets       = 12,
        .crc_errors    = 1,
        .header_errors = 0,
        .dropped       = 0,
        .ignored       = 0,
        .uptime_s      = 754,
        .heap_free     = 123456,
        .heap_min      = 98765,
        .stack_free    = 1500,
    };
    lora_p4_stats_t const zero = {0};
    char                  line[320];
    size_t n = lora_status_format(&defaults, &st, true, 4, &zero, line, sizeof(line));
    CHECK(
        strcmp(
            line,
            "869.618 MHz 62.5 kHz SF7 CR5 sync 0x12 pre 16 | c6 rx rssi -105.2 | packets 12 crc 1 hdr 0 "
            "dropped 0 ignored 0 | c6 up 754 s heap 120/96 KiB stack 1500 B | p4 dropped 0 bad 0 reg in flight 0 timeouts 0 | "
            "status 4 s ago"
        ) == 0
    );
    CHECK(n == strlen(line));

    // While a packet goes out the C6 reports "tx" (#65)
    lora_status_t sending = st;
    sending.state         = LORA_STATE_TX;
    lora_status_format(&defaults, &sending, true, 0, &zero, line, sizeof(line));
    CHECK(strstr(line, "| c6 tx "));

    lora_p4_stats_t const p4 = {.dropped = 3, .bad = 2, .reg_in_flight = 4, .reg_timeouts = 7};
    n = lora_status_format(&defaults, &st, false, 0, &p4, line, sizeof(line));
    CHECK(strstr(line, "| c6 no status yet |"));
    CHECK(strstr(line, "p4 dropped 3 bad 2 reg in flight 4 timeouts 7"));
    CHECK(n == strlen(line));
    lora_status_format(&defaults, &st, true, 0, &p4, line, sizeof(line)); // #314: the registry's pacing
    CHECK(strstr(line, "| p4 dropped 3 bad 2 reg in flight 4 timeouts 7 | status"));

    char small[10];
    n = lora_status_format(&defaults, &st, true, 4, &zero, small, sizeof(small));
    CHECK(n == strlen(small) && n < sizeof(small)); // cut short and terminated
}

static uint32_t fnv(uint32_t h, void const *p, size_t n) {
    uint8_t const *b = p;
    while (n--) {
        h = (h ^ *b++) * 16777619u;
    }
    return h;
}

static uint32_t fnv_cfg(uint32_t h, lora_config_t const *c) {
    h = fnv(h, &c->freq_hz, sizeof(c->freq_hz));
    h = fnv(h, &c->bw_hz, sizeof(c->bw_hz));
    h = fnv(h, &c->sf, 1);
    h = fnv(h, &c->cr, 1);
    h = fnv(h, &c->sync_word, 1);
    h = fnv(h, &c->preamble, sizeof(c->preamble));
    return fnv(h, &c->tx_power_dbm, 1);
}

// Each key once with its reason (#212): pins which check refuses first and the text it gives
static void cfg_reasons(void) {
    struct {
        char const *text, *why;
    } const cases[] = {
        {"frequency_mhz = \"x\"\n", "frequency_mhz must be 862-928 MHz"},
        {"frequency_mhz = 928.5\nbandwidth_khz = true\n", "frequency_mhz must be 862-928 MHz"},
        {"bandwidth_khz = 0\n", "bandwidth_khz must be a LoRa bandwidth up to 500 kHz"},
        {"bandwidth_khz = [1]\nspreading_factor = 1.0\n", "bandwidth_khz must be a LoRa bandwidth up to 500 kHz"},
        {"spreading_factor = 256\n", "spreading_factor must be an integer"},
        {"coding_rate = \"8\"\nsync_word = -1\n", "coding_rate must be an integer (5 = 4/5 ... 8 = 4/8)"},
        {"sync_word = 1.5\n", "sync_word must be an integer 0-255"},
        {"preamble = 65536\n", "preamble must be an integer 0-65535"},
        {"spreading_factor = 4\n", "values out of range (862-928 MHz, LoRa bandwidth, SF 5-12, CR 5-8, preamble 6-64)"},
        {"preamble = 5\n", "values out of range (862-928 MHz, LoRa bandwidth, SF 5-12, CR 5-8, preamble 6-64)"},
        // #452: 65535 keyed the PA for minutes per packet; both chips now refuse it, the badge keeps the default preset
        {"preamble = 65\n", "values out of range (862-928 MHz, LoRa bandwidth, SF 5-12, CR 5-8, preamble 6-64)"},
        {"preamble = 65535\n", "values out of range (862-928 MHz, LoRa bandwidth, SF 5-12, CR 5-8, preamble 6-64)"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        lora_config_t c;
        char          err[128] = "x";
        if (parse(cases[i].text, &c, err) || !same(&c, &defaults) || strcmp(err, cases[i].why) != 0) {
            printf("FAIL reason %zu: got \"%s\"\n", i, err);
            failures++;
        }
    }
    lora_config_t c;
    char          small[8] = "x";
    CHECK(!lora_cfg_parse("preamble = 5\n", 13, &c, small, sizeof(small)) && strcmp(small, "values ") == 0);
    CHECK(!lora_cfg_parse("not toml [\n", 11, &c, small, sizeof(small)) && strlen(small) == 7 && same(&c, &defaults));
    CHECK(lora_cfg_parse("sf = 1\n", 7, &c, NULL, 0) && same(&c, &defaults)); // unknown keys are ignored
    CHECK(!lora_cfg_parse("preamble = 5\n", 13, &c, NULL, 0) && same(&c, &defaults));
}

// The limits themselves (#212): each edge value is taken, the next one past it refused with its key's reason, and
// numbers are rounded to the nearest Hz, not cut
static void cfg_edges(void) {
    struct {
        char const *text;
        uint32_t    freq_hz, bw_hz;
        uint8_t     sync_word;
        uint16_t    preamble;
    } const ok[] = {
        {"frequency_mhz = 862\n", 862000000, 62500, 0x12, 16},
        {"frequency_mhz = 928\n", 928000000, 62500, 0x12, 16},
        {"bandwidth_khz = 500\n", 869618000, 500000, 0x12, 16},
        {"sync_word = 255\n", 869618000, 62500, 255, 16},
        {"preamble = 64\n", 869618000, 62500, 0x12, 64}, // LORA_PREAMBLE_MAX (#452)
        {"preamble = 6\n", 869618000, 62500, 0x12, 6},
        {"frequency_mhz = 869.6184996\n", 869618500, 62500, 0x12, 16},  // 869618499.6 Hz
        {"bandwidth_khz = 62.4996\n", 869618000, 62500, 0x12, 16},      // 62499.6 Hz
    };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        lora_config_t c;
        char          err[128] = "x";
        bool          got      = parse(ok[i].text, &c, err);
        if (!got || err[0] || c.freq_hz != ok[i].freq_hz || c.bw_hz != ok[i].bw_hz || c.sync_word != ok[i].sync_word ||
            c.preamble != ok[i].preamble) {
            printf("FAIL edge %zu not taken as written: %s (%s, %lu Hz, %lu Hz)\n", i, ok[i].text, err,
                   (unsigned long)c.freq_hz, (unsigned long)c.bw_hz);
            failures++;
        }
    }
    struct {
        char const *text, *why;
    } const past[] = {
        {"frequency_mhz = 861.999\n", "frequency_mhz must be 862-928 MHz"},
        {"frequency_mhz = 928.001\n", "frequency_mhz must be 862-928 MHz"},
        {"bandwidth_khz = 500.001\n", "bandwidth_khz must be a LoRa bandwidth up to 500 kHz"},
        {"sync_word = 256\n", "sync_word must be an integer 0-255"},
    };
    for (size_t i = 0; i < sizeof(past) / sizeof(past[0]); i++) {
        lora_config_t c;
        char          err[128] = "";
        if (parse(past[i].text, &c, err) || !same(&c, &defaults) || strcmp(err, past[i].why) != 0) {
            printf("FAIL past edge %zu: %s got \"%s\"\n", i, past[i].text, err);
            failures++;
        }
    }
}

// Malformed config text (#212): every mutation of a full file either parses to a valid config with an empty reason,
// or falls back to the defaults with a terminated reason. The digest pins every result (taken before the #212 split);
// a deliberate change to the parser changes it: print the new value and say why in the commit. tomlc17 leaks the table
// of an inline table that fails to parse (third-party): lora_link_test.lsan.supp, set by ctest, suppresses that leak
// only, so a valid inline table (a value of the wrong type) is still tested.
static void cfg_malformed(void) {
    static char const full[] = "# lora.toml\nfrequency_mhz = 869.618\nbandwidth_khz = 62.5\nspreading_factor = 8\n"
                               "coding_rate = 5\nsync_word = 0x12\npreamble = 16\n[extra]\nkey = \"v\"\n";
    mut_rng_t r;
    mut_seed(&r, 0x212);
    uint32_t h   = 2166136261u;
    size_t   bad = 0;
    for (int k = 0; k < 20000; k++) {
        uint8_t buf[sizeof(full) + 64];
        size_t  len = k == 0 ? sizeof(full) - 1 : mut_random(&r, (uint8_t const *)full, sizeof(full) - 1, buf, sizeof(buf) - 1);
        if (k == 0) {
            memcpy(buf, full, len);
        }
        buf[len] = 0; // the documented contract: NUL-terminated text, len without the NUL
        uint8_t *text = mut_exact(buf, len + 1);
        lora_config_t c;
        char          err[64];
        memset(err, 'z', sizeof(err));
        bool ok   = lora_cfg_parse((char const *)text, len, &c, err, sizeof(err));
        bool sane = memchr(err, 0, sizeof(err)) &&
                    (ok ? err[0] == 0 && lora_config_valid(&c) && c.tx_power_dbm == defaults.tx_power_dbm
                        : err[0] != 0 && same(&c, &defaults) && c.tx_power_dbm == defaults.tx_power_dbm);
        if (!sane && bad++ < 5) {
            printf("FAIL config mutation %d (ok %d): %s\n", k, ok, (char const *)text);
        }
        h = fnv(h, &ok, sizeof(ok));
        h = fnv_cfg(h, &c);
        h = fnv(h, err, strnlen(err, sizeof(err)));
        free(text);
    }
    failures += bad ? 1 : 0;
    uint32_t const pinned = 0xad0f43e6u; // #452: the reason now says "preamble 6-64", and over 64 is refused
    if (h != pinned) {
        printf("FAIL lora_cfg_parse digest 0x%08x, pinned 0x%08x\n", (unsigned)h, (unsigned)pinned);
        failures++;
    }
}

int main(void) {
    cfg_defaults_when_empty();
    cfg_skel_file_matches_defaults();
    cfg_overrides();
    cfg_bad_values_fall_back();
    cfg_reasons();
    cfg_edges();
    cfg_malformed();
    supervisor_starts_with_config();
    supervisor_reconfigures_after_c6_restart();
    supervisor_reconfigures_after_silence();
    supervisor_retries_failed_radio_slowly();
    supervisor_restarts_after_reattach();
    c6_link_across_a_reset();
    status_line_format();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("lora_link_test: all passed\n");
    return 0;
}
