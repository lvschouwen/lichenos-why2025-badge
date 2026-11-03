#include "timer_calc.h"

#include <stdio.h>
#include <string.h>

// ---- countdown ----

void tcd_init(tcd_t *t) {
    memset(t, 0, sizeof(*t));
    t->state = TCD_IDLE;
}

bool tcd_set(tcd_t *t, int64_t ms) {
    if (t->state == TCD_RUNNING || t->state == TCD_PAUSED || ms < 0 || ms > TCD_MAX_MS) {
        return false;
    }
    t->state   = TCD_IDLE;
    t->set_ms  = ms;
    t->left_ms = ms;
    return true;
}

bool tcd_start(tcd_t *t, int64_t now_ms) {
    if (t->set_ms <= 0 || t->state == TCD_RUNNING) {
        return t->state == TCD_RUNNING;
    }
    if (t->state == TCD_DONE || t->left_ms <= 0) {
        t->left_ms = t->set_ms;
    }
    t->end_ms = now_ms + t->left_ms;
    t->state  = TCD_RUNNING;
    return true;
}

void tcd_pause(tcd_t *t, int64_t now_ms) {
    if (t->state == TCD_RUNNING) {
        t->left_ms = tcd_left(t, now_ms);
        t->state   = TCD_PAUSED;
    }
}

void tcd_reset(tcd_t *t) {
    t->state   = TCD_IDLE;
    t->left_ms = t->set_ms;
}

int64_t tcd_left(tcd_t const *t, int64_t now_ms) {
    switch (t->state) {
    case TCD_RUNNING: return t->end_ms > now_ms ? t->end_ms - now_ms : 0;
    case TCD_DONE:    return 0;
    default:          return t->left_ms;
    }
}

bool tcd_poll(tcd_t *t, int64_t now_ms) {
    if (t->state == TCD_RUNNING && now_ms >= t->end_ms) {
        t->state   = TCD_DONE;
        t->left_ms = 0;
        return true;
    }
    return false;
}

// ---- stopwatch ----

void sw_init(sw_t *s) {
    memset(s, 0, sizeof(*s));
}

void sw_start(sw_t *s, int64_t now_ms) {
    if (!s->running) {
        s->running  = true;
        s->start_ms = now_ms;
    }
}

void sw_stop(sw_t *s, int64_t now_ms) {
    if (s->running) {
        s->acc_ms += now_ms - s->start_ms;
        s->running = false;
    }
}

void sw_reset(sw_t *s) {
    sw_init(s);
}

int64_t sw_elapsed(sw_t const *s, int64_t now_ms) {
    return s->acc_ms + (s->running ? now_ms - s->start_ms : 0);
}

bool sw_lap(sw_t *s, int64_t now_ms) {
    if (!s->running || s->laps >= SW_MAX_LAPS) {
        return false;
    }
    s->lap_total[s->laps++] = sw_elapsed(s, now_ms);
    return true;
}

bool sw_lap_get(sw_t const *s, size_t n, int64_t *split_ms, int64_t *total_ms) {
    if (n < 1 || n > s->laps) {
        return false;
    }
    *total_ms = s->lap_total[n - 1];
    *split_ms = *total_ms - (n > 1 ? s->lap_total[n - 2] : 0);
    return true;
}

// ---- text ----

static size_t put(size_t n, int w) {
    return w < 0 ? 0 : (size_t)w < n ? (size_t)w : n ? n - 1 : 0;
}

size_t tfmt_countdown(char *out, size_t n, int64_t ms) {
    int64_t s = ms <= 0 ? 0 : (ms + 999) / 1000;
    if (s >= 3600) {
        return put(n, snprintf(out, n, "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60)));
    }
    return put(n, snprintf(out, n, "%d:%02d", (int)(s / 60), (int)(s % 60)));
}

size_t tfmt_stopwatch(char *out, size_t n, int64_t ms) {
    int64_t t = ms < 0 ? 0 : ms / 100; // tenths
    int     tenth = (int)(t % 10);
    int64_t s = t / 10;
    if (s >= 3600) {
        return put(n, snprintf(out, n, "%d:%02d:%02d.%d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60), tenth));
    }
    return put(n, snprintf(out, n, "%02d:%02d.%d", (int)(s / 60), (int)(s % 60), tenth));
}

size_t tfmt_words(char *out, size_t n, int64_t ms) {
    int64_t s = ms <= 0 ? 0 : (ms + 500) / 1000;
    int     h = (int)(s / 3600), m = (int)(s / 60 % 60), sec = (int)(s % 60);
    if (h > 0) {
        return put(n, m ? snprintf(out, n, "%d h %d min", h, m) : snprintf(out, n, "%d h", h));
    }
    if (m > 0) {
        return put(n, sec ? snprintf(out, n, "%d min %d s", m, sec) : snprintf(out, n, "%d min", m));
    }
    return put(n, snprintf(out, n, "%d s", sec));
}

// One run of 1-2 digits starting at *p; false when none or more than 2
static bool digits(char const **p, int *value) {
    int count = 0, v = 0;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (**p - '0');
        (*p)++;
        count++;
    }
    *value = v;
    return count >= 1 && count <= 2;
}

bool tdur_parse(char const *text, int64_t *ms) {
    while (*text == ' ') {
        text++;
    }
    int part[3], parts = 0;
    char const *p = text;
    while (parts < 3) {
        if (!digits(&p, &part[parts])) {
            return false;
        }
        parts++;
        if (*p != ':') {
            break;
        }
        p++;
    }
    while (*p == ' ') {
        p++;
    }
    if (*p != '\0') {
        return false;
    }
    int64_t total;
    if (parts == 1) {
        total = part[0] * 60LL; // minutes
    } else if (parts == 2) {
        total = part[0] * 60LL + part[1];
        if (part[1] > 59) {
            return false;
        }
    } else {
        total = part[0] * 3600LL + part[1] * 60LL + part[2];
        if (part[1] > 59 || part[2] > 59) {
            return false;
        }
    }
    if (total <= 0 || total * 1000 > TCD_MAX_MS) {
        return false;
    }
    *ms = total * 1000;
    return true;
}

// ---- world clock ----

static int64_t floor_days(int64_t secs) {
    return secs >= 0 ? secs / 86400 : -((-secs + 86399) / 86400);
}

// Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm)
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t  era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t  yy  = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp  = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yy + (*m <= 2);
}

static int month_days(int64_t y, unsigned m) {
    static unsigned char const days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return days[m - 1] + (m == 2 && leap);
}

typedef struct {
    int m, w, d; // Mm.w.d
    int secs;    // wall clock time of the change, default 02:00:00
} rule_t;

typedef struct {
    int    std_off; // seconds east of UTC
    int    dst_off;
    bool   has_dst;
    rule_t start, end;
} zone_t;

static bool skip_name(char const **p) {
    char const *s = *p;
    while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) {
        s++;
    }
    if (s - *p < 3) {
        return false;
    }
    *p = s;
    return true;
}

// [+-]hh[:mm[:ss]] in seconds, as written (POSIX: positive = west)
static bool parse_hms(char const **p, int *secs) {
    int sign = 1;
    if (**p == '-' || **p == '+') {
        sign = **p == '-' ? -1 : 1;
        (*p)++;
    }
    int v[3] = {0, 0, 0}, i = 0;
    for (; i < 3; i++) {
        int count = 0;
        while (**p >= '0' && **p <= '9') {
            v[i] = v[i] * 10 + (**p - '0');
            (*p)++;
            count++;
        }
        if (count == 0 || count > 3) {
            return false;
        }
        if (**p != ':') {
            break;
        }
        (*p)++;
    }
    *secs = sign * (v[0] * 3600 + v[1] * 60 + v[2]);
    return true;
}

static bool parse_rule(char const **p, rule_t *r) {
    if (**p != 'M') {
        return false;
    }
    (*p)++;
    char const *s = *p;
    int v[3];
    for (int i = 0; i < 3; i++) {
        v[i] = 0;
        int count = 0;
        while (*s >= '0' && *s <= '9') {
            v[i] = v[i] * 10 + (*s - '0');
            s++;
            count++;
        }
        if (count == 0 || (i < 2 && *s != '.')) {
            return false;
        }
        if (i < 2) {
            s++;
        }
    }
    if (v[0] < 1 || v[0] > 12 || v[1] < 1 || v[1] > 5 || v[2] > 6) {
        return false;
    }
    r->m = v[0];
    r->w = v[1];
    r->d = v[2];
    r->secs = 2 * 3600;
    if (*s == '/') {
        s++;
        if (!parse_hms(&s, &r->secs)) {
            return false;
        }
    }
    *p = s;
    return true;
}

static bool parse_zone(char const *posix, zone_t *z) {
    memset(z, 0, sizeof(*z));
    char const *p = posix;
    int off;
    if (!skip_name(&p) || !parse_hms(&p, &off)) {
        return false;
    }
    z->std_off = -off;
    if (*p == '\0') {
        return true;
    }
    if (!skip_name(&p)) {
        return false;
    }
    z->dst_off = z->std_off + 3600;
    if (*p != ',' && *p != '\0') {
        if (!parse_hms(&p, &off)) {
            return false;
        }
        z->dst_off = -off;
    }
    if (*p != ',') { // a DST name without rules: this does not guess the default ones
        return *p == '\0';
    }
    p++;
    if (!parse_rule(&p, &z->start) || *p++ != ',' || !parse_rule(&p, &z->end) || *p != '\0') {
        return false;
    }
    z->has_dst = true;
    return true;
}

// The wall-clock seconds since 1970 (as if the wall clock were UTC) of a rule's change in year y
static int64_t rule_wall(int64_t y, rule_t const *r) {
    int64_t first   = days_from_civil(y, (unsigned)r->m, 1);
    int     wd1     = (int)(((first % 7) + 11) % 7); // 1970-01-01 was a Thursday (4); 0 = Sunday
    int     day     = 1 + (r->d - wd1 + 7) % 7 + 7 * (r->w - 1);
    while (day > month_days(y, (unsigned)r->m)) {
        day -= 7; // week 5 means the last
    }
    return (first + day - 1) * 86400 + r->secs;
}

static bool in_dst(zone_t const *z, int64_t utc) {
    if (!z->has_dst) {
        return false;
    }
    int64_t y;
    unsigned m, d;
    civil_from_days(floor_days(utc + z->std_off), &y, &m, &d);
    int64_t start = rule_wall(y, &z->start) - z->std_off;
    int64_t end   = rule_wall(y, &z->end) - z->dst_off;
    return start < end ? (utc >= start && utc < end) : (utc >= start || utc < end);
}

bool wc_local(char const *posix, int64_t utc, wc_local_t *out) {
    zone_t z;
    if (!posix || !parse_zone(posix, &z)) {
        return false;
    }
    out->dst      = in_dst(&z, utc);
    out->offset_s = out->dst ? z.dst_off : z.std_off;
    int64_t local = utc + out->offset_s;
    int64_t days  = floor_days(local);
    int64_t secs  = local - days * 86400;
    out->wday     = (int)(((days % 7) + 11) % 7);
    out->hour     = (int)(secs / 3600);
    out->min      = (int)(secs / 60 % 60);
    return true;
}

size_t wc_fmt(char *out, size_t n, wc_local_t const *l) {
    static char const *const day[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    return put(n, snprintf(out, n, "%s %02d:%02d", day[l->wday], l->hour, l->min));
}
