// Host test: the Timer app's countdown, stopwatch, text and world clock (#152)
#include "timer_calc.h"

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

static void text_is(size_t (*fn)(char *, size_t, int64_t), int64_t ms, char const *want) {
    char out[32];
    fn(out, sizeof(out), ms);
    if (strcmp(out, want) != 0) {
        printf("FAIL %lld ms: \"%s\" want \"%s\"\n", (long long)ms, out, want);
        failures++;
    }
}

static void countdown(void) {
    tcd_t t;
    tcd_init(&t);
    CHECK(!tcd_start(&t, 0)); // nothing set
    CHECK(tcd_set(&t, 5000));
    CHECK(tcd_left(&t, 0) == 5000);
    CHECK(tcd_start(&t, 1000));
    CHECK(t.state == TCD_RUNNING);
    CHECK(tcd_left(&t, 3000) == 3000);
    CHECK(!tcd_set(&t, 9000)); // not while it runs
    CHECK(!tcd_poll(&t, 5999));
    tcd_pause(&t, 4000); // ends at 6000: 2000 left
    CHECK(t.state == TCD_PAUSED && tcd_left(&t, 999999) == 2000);
    CHECK(!tcd_poll(&t, 999999)); // paused: never fires
    CHECK(!tcd_set(&t, 9000));
    CHECK(tcd_start(&t, 10000));
    CHECK(tcd_left(&t, 11000) == 1000);
    CHECK(!tcd_poll(&t, 11999));
    CHECK(tcd_poll(&t, 12000)); // exactly once
    CHECK(!tcd_poll(&t, 12001));
    CHECK(t.state == TCD_DONE && tcd_left(&t, 20000) == 0);
    CHECK(tcd_start(&t, 20000)); // again from the full duration
    CHECK(tcd_left(&t, 20000) == 5000);
    tcd_reset(&t);
    CHECK(t.state == TCD_IDLE && tcd_left(&t, 0) == 5000);
    CHECK(tcd_set(&t, 7000) && tcd_left(&t, 0) == 7000);
    CHECK(!tcd_set(&t, TCD_MAX_MS + 1));
    CHECK(tcd_set(&t, TCD_MAX_MS));
    CHECK(tcd_set(&t, 0) && !tcd_start(&t, 0));
    // polled late (the badge was busy): still fires once, left is 0
    tcd_set(&t, 1000);
    tcd_start(&t, 0);
    CHECK(tcd_poll(&t, 50000) && tcd_left(&t, 50000) == 0);
    // starting a running countdown changes nothing
    tcd_set(&t, 10000);
    tcd_start(&t, 0);
    CHECK(tcd_start(&t, 4000) && tcd_left(&t, 4000) == 6000);
}

static void stopwatch(void) {
    sw_t s;
    sw_init(&s);
    CHECK(sw_elapsed(&s, 500) == 0);
    CHECK(!sw_lap(&s, 500)); // not running
    sw_start(&s, 1000);
    CHECK(sw_elapsed(&s, 1500) == 500);
    sw_start(&s, 1700); // already running: no restart
    CHECK(sw_elapsed(&s, 2000) == 1000);
    CHECK(sw_lap(&s, 2000));
    CHECK(sw_lap(&s, 3500));
    sw_stop(&s, 4000);
    CHECK(sw_elapsed(&s, 99999) == 3000); // frozen
    sw_start(&s, 10000);
    CHECK(sw_elapsed(&s, 10500) == 3500);
    CHECK(sw_lap(&s, 10500));
    int64_t split, total;
    CHECK(sw_lap_get(&s, 1, &split, &total) && split == 1000 && total == 1000);
    CHECK(sw_lap_get(&s, 2, &split, &total) && split == 1500 && total == 2500);
    CHECK(sw_lap_get(&s, 3, &split, &total) && split == 1000 && total == 3500);
    CHECK(!sw_lap_get(&s, 4, &split, &total) && !sw_lap_get(&s, 0, &split, &total));
    sw_reset(&s);
    CHECK(!s.running && s.laps == 0 && sw_elapsed(&s, 20000) == 0);
    sw_start(&s, 0);
    for (int i = 0; i < SW_MAX_LAPS; i++) {
        CHECK(sw_lap(&s, i * 10));
    }
    CHECK(!sw_lap(&s, 99999));
}

static void texts(void) {
    text_is(tfmt_countdown, 300000, "5:00");
    text_is(tfmt_countdown, 7000, "0:07");
    text_is(tfmt_countdown, 6001, "0:07"); // rounds up
    text_is(tfmt_countdown, 1, "0:01");
    text_is(tfmt_countdown, 0, "0:00");
    text_is(tfmt_countdown, -5, "0:00");
    text_is(tfmt_countdown, 3723000, "1:02:03");
    text_is(tfmt_countdown, TCD_MAX_MS, "99:59:59");
    text_is(tfmt_stopwatch, 0, "00:00.0");
    text_is(tfmt_stopwatch, 12399, "00:12.3");
    text_is(tfmt_stopwatch, 59999, "00:59.9");
    text_is(tfmt_stopwatch, 60000, "01:00.0");
    text_is(tfmt_stopwatch, 3723400, "1:02:03.4");
    text_is(tfmt_words, 300000, "5 min");
    text_is(tfmt_words, 90000, "1 min 30 s");
    text_is(tfmt_words, 45000, "45 s");
    text_is(tfmt_words, 7500000, "2 h 5 min");
    text_is(tfmt_words, 3600000, "1 h");
    char tiny[4];
    tfmt_countdown(tiny, sizeof(tiny), 300000);
    CHECK(strcmp(tiny, "5:0") == 0);
}

static void duration(void) {
    int64_t ms = 0;
    CHECK(tdur_parse("5", &ms) && ms == 300000);
    CHECK(tdur_parse("  10 ", &ms) && ms == 600000);
    CHECK(tdur_parse("5:30", &ms) && ms == 330000);
    CHECK(tdur_parse("0:45", &ms) && ms == 45000);
    CHECK(tdur_parse("1:00:00", &ms) && ms == 3600000);
    CHECK(tdur_parse("1:02:03", &ms) && ms == 3723000);
    CHECK(tdur_parse("99:59:59", &ms) && ms == TCD_MAX_MS);
    CHECK(!tdur_parse("", &ms));
    CHECK(!tdur_parse("0", &ms));
    CHECK(!tdur_parse("0:00", &ms));
    CHECK(!tdur_parse("abc", &ms));
    CHECK(!tdur_parse("5:60", &ms));
    CHECK(!tdur_parse("1:60:00", &ms));
    CHECK(!tdur_parse("1:00:60", &ms));
    CHECK(!tdur_parse("100", &ms)); // more than two digits
    CHECK(!tdur_parse("5:", &ms));
    CHECK(!tdur_parse(":30", &ms));
    CHECK(!tdur_parse("1:2:3:4", &ms));
    CHECK(!tdur_parse("-5", &ms));
    CHECK(!tdur_parse("5 min", &ms));
    CHECK(!tdur_parse("1.5", &ms));
}

#define CET "CET-1CEST,M3.5.0,M10.5.0/3"
#define NYC "EST5EDT,M3.2.0,M11.1.0"
#define SYD "AEST-10AEDT,M10.1.0,M4.1.0/3"
#define LON "GMT0BST,M3.5.0/1,M10.5.0"

static void zone(char const *posix, int64_t utc, char const *want, int offset, bool dst) {
    wc_local_t l;
    char       out[16];
    if (!wc_local(posix, utc, &l)) {
        printf("FAIL %s at %lld: not parsed\n", posix, (long long)utc);
        failures++;
        return;
    }
    wc_fmt(out, sizeof(out), &l);
    if (strcmp(out, want) != 0 || l.offset_s != offset || l.dst != dst) {
        printf("FAIL %s at %lld: %s off %d dst %d, want %s off %d dst %d\n", posix, (long long)utc, out, l.offset_s,
               (int)l.dst, want, offset, (int)dst);
        failures++;
    }
}

static void world(void) {
    // Values from the IANA database (zoneinfo) for the same instants
    zone("UTC0", 1793707200, "Tue 12:00", 0, false);
    zone(CET, 1793707200, "Tue 13:00", 3600, false);
    zone(NYC, 1793707200, "Tue 07:00", -18000, false);
    zone(SYD, 1793707200, "Tue 23:00", 39600, true);
    zone("NZST-12NZDT,M9.5.0,M4.1.0/3", 1793707200, "Wed 01:00", 46800, true); // a new day over there
    zone("PST8PDT,M3.2.0,M11.1.0", 1793707200, "Tue 04:00", -28800, false);
    zone("EET-2EEST,M3.5.0/3,M10.5.0/4", 1793707200, "Tue 14:00", 7200, false);
    // northern summer
    zone(CET, 1782865800, "Wed 02:30", 7200, true);
    zone(LON, 1782865800, "Wed 01:30", 3600, true);
    zone(NYC, 1782865800, "Tue 20:30", -14400, true);
    zone(SYD, 1782865800, "Wed 10:30", 36000, false);
    // the changeover instants (2026-03-29 01:00 UTC, 2026-10-25 01:00 UTC)
    zone(CET, 1774745999, "Sun 01:59", 3600, false);
    zone(CET, 1774746000, "Sun 03:00", 7200, true);
    zone(CET, 1792889999, "Sun 02:59", 7200, true);
    zone(CET, 1792890000, "Sun 02:00", 3600, false);
    zone(LON, 1774746000, "Sun 02:00", 3600, true);
    zone(LON, 1792890000, "Sun 01:00", 0, false);
    zone("EET-2EEST,M3.5.0/3,M10.5.0/4", 1774746000, "Sun 04:00", 10800, true);
    zone("EET-2EEST,M3.5.0/3,M10.5.0/4", 1792890000, "Sun 03:00", 7200, false);
    zone(SYD, 1774745999, "Sun 11:59", 39600, true); // still summer time in March
    // across midnight, and before 1970
    zone(CET, 1768519800, "Fri 00:30", 3600, false);
    zone(NYC, 1768519800, "Thu 18:30", -18000, false);
    zone(CET, -3600, "Thu 00:00", 3600, false);
    // other shapes
    zone("JST-9", 0, "Thu 09:00", 32400, false);
    zone("IST-5:30", 0, "Thu 05:30", 19800, false);
    wc_local_t l;
    CHECK(!wc_local("", 0, &l));
    CHECK(!wc_local("UTC", 0, &l));        // no offset
    CHECK(!wc_local("CET-1CEST,M13.1.0,M10.5.0", 0, &l));
    CHECK(!wc_local("CET-1CEST,J60,J300", 0, &l)); // Julian day rules are not supported
    CHECK(!wc_local(NULL, 0, &l));
}

int main(void) {
    countdown();
    stopwatch();
    texts();
    duration();
    world();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("timer_calc_test: ok\n");
    return 0;
}
