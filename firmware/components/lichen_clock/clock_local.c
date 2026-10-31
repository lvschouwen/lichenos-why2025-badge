#include "clock_local.h"

#include <time.h>

#define DAY_S 86400LL

int clock_days_in_month(int year, int month) {
    static int const days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : days[month - 1];
}

// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil): no mktime, whose guess at DST
// for an ambiguous hour is exactly what this file has to decide itself
static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t  era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

// What the wall clock reads at t, as seconds of a "UTC" day count: t plus the zone's offset then
static int64_t wall_at(int64_t t) {
    time_t    tt = (time_t)t;
    struct tm l;
    if (!localtime_r(&tt, &l)) {
        return INT64_MIN;
    }
    return days_from_civil(l.tm_year + 1900LL, l.tm_mon + 1, l.tm_mday) * DAY_S + l.tm_hour * 3600LL +
           l.tm_min * 60LL + l.tm_sec;
}

static bool valid_fields(int y, int mo, int d, int h, int mi) {
    return y >= 1970 && y <= 9999 && mo >= 1 && mo <= 12 && d >= 1 && d <= clock_days_in_month(y, mo) && h >= 0 &&
           h < 24 && mi >= 0 && mi < 60;
}

// Two candidates, one with the offset a day before and one with the offset a day after (zones change at most once
// in two days): a candidate is real when the wall clock reads `wall` at it. Both real and different: the hour comes
// twice; neither: it falls in the spring gap.
clock_local_t clock_local_to_utc(int y, int mo, int d, int h, int mi, int64_t *utc) {
    if (!valid_fields(y, mo, d, h, mi)) {
        return CLOCK_LOCAL_BAD_DATE;
    }
    int64_t wall   = days_from_civil(y, mo, d) * DAY_S + h * 3600LL + mi * 60LL;
    int64_t before = wall_at(wall - DAY_S) - (wall - DAY_S);
    int64_t after  = wall_at(wall + DAY_S) - (wall + DAY_S);
    int64_t a = wall - before, b = wall - after;
    bool    a_ok = wall_at(a) == wall, b_ok = wall_at(b) == wall;
    if (a_ok && b_ok && a != b) {
        *utc = a < b ? a : b; // the first instance: summer time
        return CLOCK_LOCAL_TWICE;
    }
    if (a_ok || b_ok) {
        *utc = a_ok ? a : b;
        return CLOCK_LOCAL_OK;
    }
    *utc = a; // the offset before the change: 02:30 reads 03:30 summer time
    return CLOCK_LOCAL_GAP;
}

char const *clock_local_hint(clock_local_t how) {
    switch (how) {
        case CLOCK_LOCAL_GAP: return "that hour is skipped (clocks go forward): read as summer time";
        case CLOCK_LOCAL_TWICE: return "that hour comes twice (clocks go back): the first, summer time";
        default: return NULL;
    }
}

char const *clock_back_hint(int64_t now, int64_t set_to) {
    return set_to < now - CLOCK_BACK_HINT_S ? "earlier than before: mesh nodes ignore our adverts until then" : NULL;
}

// n digits at *p into *out; advances *p
static bool digits(char const **p, int n, int *out) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        char c = (*p)[i];
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    *p += n;
    *out = v;
    return true;
}

static bool lit(char const **p, char c) {
    if (**p != c) {
        return false;
    }
    (*p)++;
    return true;
}

// Z, +HH:MM, -HH:MM, +HHMM, or nothing; *zoned false for nothing. Leaves *p at the first character after it.
static bool parse_offset(char const **p, bool *zoned, int *off_s) {
    *zoned = true;
    *off_s = 0;
    if (lit(p, 'Z')) {
        return true;
    }
    char sign = **p;
    if (sign != '+' && sign != '-') {
        *zoned = false;
        return true;
    }
    (*p)++;
    int oh, om;
    if (!digits(p, 2, &oh)) {
        return false;
    }
    lit(p, ':');
    if (!digits(p, 2, &om) || oh > 14 || om > 59) {
        return false;
    }
    *off_s = (oh * 3600 + om * 60) * (sign == '-' ? -1 : 1);
    return true;
}

bool clock_parse_iso(char const *s, int64_t *utc, clock_local_t *how) {
    if (!s) {
        return false;
    }
    while (*s == ' ') {
        s++;
    }
    int  y, mo, d, h, mi, sec = 0, off_s;
    bool zoned;
    if (!digits(&s, 4, &y) || !lit(&s, '-') || !digits(&s, 2, &mo) || !lit(&s, '-') || !digits(&s, 2, &d) ||
        !(lit(&s, 'T') || lit(&s, ' ')) || !digits(&s, 2, &h) || !lit(&s, ':') || !digits(&s, 2, &mi)) {
        return false;
    }
    if (lit(&s, ':') && (!digits(&s, 2, &sec) || sec > 59)) {
        return false;
    }
    while (*s == ' ') {
        s++;
    }
    if (!parse_offset(&s, &zoned, &off_s)) {
        return false;
    }
    while (*s == ' ') {
        s++;
    }
    if (*s || !valid_fields(y, mo, d, h, mi)) {
        return false;
    }
    clock_local_t kind = CLOCK_LOCAL_OK;
    int64_t       t;
    if (zoned) {
        t = days_from_civil(y, mo, d) * DAY_S + h * 3600LL + mi * 60LL - off_s;
    } else {
        kind = clock_local_to_utc(y, mo, d, h, mi, &t);
    }
    *utc = t + sec;
    if (how) {
        *how = kind;
    }
    return true;
}
