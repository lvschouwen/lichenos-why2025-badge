// Host test: a local date and time to UTC in the selected zone, DST edges included, and "fw clock set" parsing (#64)
#include "clock_local.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void zone(char const *posix) {
    setenv("TZ", posix, 1);
    tzset();
}

// UTC seconds for a UTC date and time, independent of the code under test (timegm is in glibc)
static int64_t utc(int y, int mo, int d, int h, int mi) {
    struct tm t = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi};
    return (int64_t)timegm(&t);
}

static clock_local_t to_utc(int y, int mo, int d, int h, int mi, int64_t *out) {
    *out = -1;
    return clock_local_to_utc(y, mo, d, h, mi, out);
}

static void amsterdam(void) {
    int64_t t;
    zone("CET-1CEST,M3.5.0,M10.5.0/3"); // TZ_DEFAULT_POSIX
    // autumn 2026: 03:00 CEST becomes 02:00 CET on Sunday 25 October
    CHECK(to_utc(2026, 10, 25, 1, 59, &t) == CLOCK_LOCAL_OK && t == utc(2026, 10, 24, 23, 59));
    CHECK(to_utc(2026, 10, 25, 2, 30, &t) == CLOCK_LOCAL_TWICE && t == utc(2026, 10, 25, 0, 30)); // first: summer
    CHECK(to_utc(2026, 10, 25, 2, 0, &t) == CLOCK_LOCAL_TWICE && t == utc(2026, 10, 25, 0, 0));
    CHECK(to_utc(2026, 10, 25, 3, 0, &t) == CLOCK_LOCAL_OK && t == utc(2026, 10, 25, 2, 0));
    // spring 2027: 02:00 CET becomes 03:00 CEST on Sunday 28 March
    CHECK(to_utc(2027, 3, 28, 1, 59, &t) == CLOCK_LOCAL_OK && t == utc(2027, 3, 28, 0, 59));
    CHECK(to_utc(2027, 3, 28, 2, 30, &t) == CLOCK_LOCAL_GAP && t == utc(2027, 3, 28, 1, 30)); // reads 03:30 CEST
    CHECK(to_utc(2027, 3, 28, 3, 0, &t) == CLOCK_LOCAL_OK && t == utc(2027, 3, 28, 1, 0));
    // dates
    CHECK(to_utc(2028, 2, 29, 12, 0, &t) == CLOCK_LOCAL_OK && t == utc(2028, 2, 29, 11, 0));
    CHECK(to_utc(2027, 2, 29, 12, 0, &t) == CLOCK_LOCAL_BAD_DATE);
    CHECK(to_utc(2026, 4, 31, 12, 0, &t) == CLOCK_LOCAL_BAD_DATE);
    CHECK(to_utc(2026, 13, 1, 12, 0, &t) == CLOCK_LOCAL_BAD_DATE);
    CHECK(to_utc(2026, 0, 1, 12, 0, &t) == CLOCK_LOCAL_BAD_DATE);
    CHECK(to_utc(2026, 1, 0, 12, 0, &t) == CLOCK_LOCAL_BAD_DATE);
    CHECK(to_utc(2026, 1, 1, 24, 0, &t) == CLOCK_LOCAL_BAD_DATE);
    CHECK(to_utc(2026, 1, 1, 0, 60, &t) == CLOCK_LOCAL_BAD_DATE);
    CHECK(to_utc(2026, 1, 1, -1, 0, &t) == CLOCK_LOCAL_BAD_DATE);
    CHECK(to_utc(2026, 7, 1, 0, 0, &t) == CLOCK_LOCAL_OK && t == utc(2026, 6, 30, 22, 0)); // summer, a day back
    CHECK(clock_days_in_month(2028, 2) == 29 && clock_days_in_month(2027, 2) == 28 &&
          clock_days_in_month(2100, 2) == 28 && clock_days_in_month(2000, 2) == 29);
    CHECK(clock_days_in_month(2026, 12) == 31 && clock_days_in_month(2026, 11) == 30);
}

static void other_zones(void) {
    int64_t t;
    zone("UTC0");
    CHECK(to_utc(2026, 10, 25, 2, 30, &t) == CLOCK_LOCAL_OK && t == utc(2026, 10, 25, 2, 30));
    CHECK(to_utc(2027, 3, 28, 2, 30, &t) == CLOCK_LOCAL_OK && t == utc(2027, 3, 28, 2, 30));
    zone("EST5EDT,M3.2.0,M11.1.0"); // New York: 8 March / 1 November 2026
    CHECK(to_utc(2026, 3, 8, 2, 30, &t) == CLOCK_LOCAL_GAP && t == utc(2026, 3, 8, 7, 30));
    CHECK(to_utc(2026, 11, 1, 1, 30, &t) == CLOCK_LOCAL_TWICE && t == utc(2026, 11, 1, 5, 30));
    CHECK(to_utc(2026, 7, 4, 12, 0, &t) == CLOCK_LOCAL_OK && t == utc(2026, 7, 4, 16, 0));
    zone("AEST-10AEDT,M10.1.0,M4.1.0/3"); // Sydney, southern DST: 4 October 2026 forward, 5 April 2026 back
    CHECK(to_utc(2026, 10, 4, 2, 30, &t) == CLOCK_LOCAL_GAP && t == utc(2026, 10, 3, 16, 30));
    CHECK(to_utc(2026, 4, 5, 2, 30, &t) == CLOCK_LOCAL_TWICE && t == utc(2026, 4, 4, 15, 30));
    CHECK(to_utc(2026, 1, 15, 12, 0, &t) == CLOCK_LOCAL_OK && t == utc(2026, 1, 15, 1, 0)); // summer there
}

static void parse(void) {
    int64_t       t;
    clock_local_t how;
    zone("CET-1CEST,M3.5.0,M10.5.0/3");
    CHECK(clock_parse_iso("2026-10-31T12:00Z", &t, &how) && t == utc(2026, 10, 31, 12, 0) && how == CLOCK_LOCAL_OK);
    CHECK(clock_parse_iso("2026-10-31 12:00:30Z", &t, NULL) && t == utc(2026, 10, 31, 12, 0) + 30);
    CHECK(clock_parse_iso("2026-10-31T12:00+02:00", &t, NULL) && t == utc(2026, 10, 31, 10, 0));
    CHECK(clock_parse_iso("2026-10-31T12:00-05:00", &t, NULL) && t == utc(2026, 10, 31, 17, 0));
    CHECK(clock_parse_iso("2026-10-31 12:00 -05:00", &t, NULL) && t == utc(2026, 10, 31, 17, 0)); // a space before the offset
    CHECK(clock_parse_iso("2026-10-31T12:00:05+0130", &t, NULL) && t == utc(2026, 10, 31, 10, 30) + 5);
    CHECK(clock_parse_iso("2026-10-31T12:00", &t, &how) && t == utc(2026, 10, 31, 11, 0) && how == CLOCK_LOCAL_OK);
    CHECK(clock_parse_iso("2026-10-31T12:00:59", &t, NULL) && t == utc(2026, 10, 31, 11, 0) + 59);
    CHECK(clock_parse_iso("2026-10-25T02:30", &t, &how) && how == CLOCK_LOCAL_TWICE && t == utc(2026, 10, 25, 0, 30));
    CHECK(clock_parse_iso("2027-03-28 02:30", &t, &how) && how == CLOCK_LOCAL_GAP && t == utc(2027, 3, 28, 1, 30));
    CHECK(clock_parse_iso("  2026-10-31T12:00Z  ", &t, NULL) && t == utc(2026, 10, 31, 12, 0)); // console spaces
    CHECK(clock_parse_iso("2040-01-01T00:00Z", &t, NULL) && t == utc(2040, 1, 1, 0, 0) && t > 2147483647LL); // 2038+
    // junk
    CHECK(!clock_parse_iso("", &t, NULL));
    CHECK(!clock_parse_iso("now", &t, NULL));
    CHECK(!clock_parse_iso("2026-10-31", &t, NULL));
    CHECK(!clock_parse_iso("2026-10-31T12", &t, NULL));
    CHECK(!clock_parse_iso("2026-10-31X12:00", &t, NULL));
    CHECK(!clock_parse_iso("2026-1-31T12:00", &t, NULL));
    CHECK(!clock_parse_iso("2026-10-31T12:00Zjunk", &t, NULL));
    CHECK(!clock_parse_iso("2026-10-31T12:00+2", &t, NULL));
    CHECK(!clock_parse_iso("2026-10-31T12:00+25:00", &t, NULL));
    CHECK(!clock_parse_iso("2026-10-31T12:00:60", &t, NULL));
    CHECK(!clock_parse_iso("2026-02-30T12:00Z", &t, NULL));
    CHECK(!clock_parse_iso("2026-10-31T24:00Z", &t, NULL));
    CHECK(!clock_parse_iso("+026-10-31T12:00Z", &t, NULL));
    CHECK(!clock_parse_iso(NULL, &t, NULL));
}

static void hints(void) {
    CHECK(clock_local_hint(CLOCK_LOCAL_OK) == NULL && clock_local_hint(CLOCK_LOCAL_BAD_DATE) == NULL);
    CHECK(clock_local_hint(CLOCK_LOCAL_GAP) && clock_local_hint(CLOCK_LOCAL_TWICE));
    CHECK(clock_back_hint(10000, 10000 - 300) == NULL); // 5 min back: fine
    CHECK(clock_back_hint(10000, 10000 - 301) != NULL);
    CHECK(clock_back_hint(10000, 99999) == NULL); // forward
    CHECK(clock_back_hint(0, 1793000000LL) == NULL); // a cold clock at 1970
}

int main(void) {
    hints();
    amsterdam();
    other_zones();
    parse();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("clock_local_test: ok\n");
    return 0;
}
