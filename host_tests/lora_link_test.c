// Host tests for the P4 LoRa link logic: config file, supervisor, status line
#include "lora_link.h"

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
    char   line[256];
    size_t n = lora_status_format(&defaults, &st, true, 4, 0, 0, line, sizeof(line));
    CHECK(
        strcmp(
            line,
            "869.618 MHz 62.5 kHz SF7 CR5 sync 0x12 pre 16 | c6 rx rssi -105.2 | packets 12 crc 1 hdr 0 "
            "dropped 0 ignored 0 | c6 up 754 s heap 120/96 KiB stack 1500 B | p4 dropped 0 bad 0 | status 4 s ago"
        ) == 0
    );
    CHECK(n == strlen(line));

    // While a packet goes out the C6 reports "tx" (#65)
    lora_status_t sending = st;
    sending.state         = LORA_STATE_TX;
    lora_status_format(&defaults, &sending, true, 0, 0, 0, line, sizeof(line));
    CHECK(strstr(line, "| c6 tx "));

    n = lora_status_format(&defaults, &st, false, 0, 3, 2, line, sizeof(line));
    CHECK(strstr(line, "| c6 no status yet |"));
    CHECK(strstr(line, "p4 dropped 3 bad 2"));
    CHECK(n == strlen(line));

    char small[10];
    n = lora_status_format(&defaults, &st, true, 4, 0, 0, small, sizeof(small));
    CHECK(n <= 9);
    CHECK(small[n] == '\0');
}

int main(void) {
    cfg_defaults_when_empty();
    cfg_skel_file_matches_defaults();
    cfg_overrides();
    cfg_bad_values_fall_back();
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
