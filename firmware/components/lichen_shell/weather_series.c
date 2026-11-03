// Weather (#155): see weather_series.h.
#include "weather_series.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define AIR_MIN_VALID  15   // minutes of gas readings before a trend means anything (the heater needs warm-up)
#define AIR_RECENT     5    // the newest readings that count as "now"
#define AIR_BETTER_PCT 110
#define AIR_WORSE_PCT  90

void wx_ring_init(wx_ring_t *r, wx_sample_t *storage, size_t cap) {
    r->buf   = storage;
    r->cap   = cap;
    r->head  = 0;
    r->count = 0;
}

void wx_ring_push(wx_ring_t *r, wx_sample_t const *s) {
    if (!r->buf || r->cap == 0) {
        return;
    }
    r->buf[r->head] = *s;
    r->head         = (r->head + 1) % r->cap;
    if (r->count < r->cap) {
        r->count++;
    }
}

size_t wx_series(wx_ring_t const *r, wx_qty_t q, float offset_k, float *out, size_t max) {
    if (r->cap == 0) { // never initialised (no memory at start): nothing to show, and no % 0
        return 0;
    }
    size_t n     = r->count < max ? r->count : max;
    size_t skip  = r->count - n; // the oldest ones do not fit: keep the newest
    size_t start = (r->head + r->cap - r->count + skip) % r->cap;
    for (size_t i = 0; i < n; i++) {
        float v = r->buf[(start + i) % r->cap].v[q];
        out[i]  = (q == WX_TEMP && !isnan(v)) ? v + offset_k : v;
    }
    return n;
}

bool wx_minmax(float const *v, size_t n, float *lo, float *hi) {
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        if (isnan(v[i])) {
            continue;
        }
        if (!any || v[i] < *lo) {
            *lo = v[i];
        }
        if (!any || v[i] > *hi) {
            *hi = v[i];
        }
        any = true;
    }
    return any;
}

static float min_span(wx_qty_t q, float mid) {
    switch (q) {
    case WX_TEMP:  return 2.0f;
    case WX_HUM:   return 10.0f;
    case WX_PRESS: return 4.0f;
    default:       return fabsf(mid) * 0.4f > 10.0f ? fabsf(mid) * 0.4f : 10.0f; // gas: relative to its level
    }
}

bool wx_range(float const *v, size_t n, wx_qty_t q, float *lo, float *hi) {
    float a, b;
    if (!wx_minmax(v, n, &a, &b)) {
        return false;
    }
    float mid  = (a + b) / 2;
    float span = b - a;
    float want = min_span(q, mid);
    if (span < want) {
        span = want;
    }
    span *= 1.1f; // 5% above and below
    *lo = mid - span / 2;
    *hi = mid + span / 2;
    return true;
}

int32_t wx_chart_value(float v) {
    return (int32_t)lroundf(v * 10.0f);
}

wx_air_t wx_air_trend(float const *gas, size_t n) {
    size_t valid = 0;
    for (size_t i = 0; i < n; i++) {
        valid += !isnan(gas[i]) && gas[i] > 0;
    }
    if (valid < AIR_MIN_VALID || n <= AIR_RECENT) {
        return WX_AIR_LEARNING;
    }
    double recent = 0, before = 0;
    size_t nr = 0, nb = 0;
    for (size_t i = 0; i < n; i++) {
        if (isnan(gas[i]) || gas[i] <= 0) {
            continue;
        }
        if (i >= n - AIR_RECENT) {
            recent += gas[i];
            nr++;
        } else {
            before += gas[i];
            nb++;
        }
    }
    if (nr < 2 || nb < AIR_MIN_VALID - AIR_RECENT) {
        return WX_AIR_LEARNING;
    }
    double ratio = (recent / (double)nr) / (before / (double)nb) * 100.0;
    return ratio >= AIR_BETTER_PCT ? WX_AIR_BETTER : ratio <= AIR_WORSE_PCT ? WX_AIR_WORSE : WX_AIR_SAME;
}

char const *wx_air_text(wx_air_t t) {
    switch (t) {
    case WX_AIR_BETTER: return "Cleaner than usual";
    case WX_AIR_WORSE:  return "Worse than usual";
    case WX_AIR_SAME:   return "About as usual";
    default:            return "Learning this room...";
    }
}

size_t wx_fmt_value(char *out, size_t cap, wx_qty_t q, float v) {
    if (isnan(v)) {
        return (size_t)snprintf(out, cap, "no reading");
    }
    switch (q) {
    case WX_TEMP:  return (size_t)snprintf(out, cap, "%.1f \xC2\xB0""C", (double)v);
    case WX_HUM:   return (size_t)snprintf(out, cap, "%.0f%%", (double)v);
    case WX_PRESS: return (size_t)snprintf(out, cap, "%.0f hPa", (double)v);
    default:       return (size_t)snprintf(out, cap, "%.0f kOhm", (double)v);
    }
}

char const *wx_qty_name(wx_qty_t q) {
    static char const *const names[WX_QTY] = {"Temperature", "Humidity", "Pressure", "Air quality"};
    return q < WX_QTY ? names[q] : "";
}

void wx_span_text(char *out, size_t cap, size_t n) {
    size_t min = n > 0 ? n - 1 : 0; // n samples are n-1 minutes apart
    if (min == 0) {
        snprintf(out, cap, "now");
    } else if (min < 60) {
        snprintf(out, cap, "%u min", (unsigned)min);
    } else if (min % 60 == 0) {
        snprintf(out, cap, "%u h", (unsigned)(min / 60));
    } else {
        snprintf(out, cap, "%u h %u min", (unsigned)(min / 60), (unsigned)(min % 60));
    }
}

int wx_offset_half_from_byte(uint8_t b) {
    int half = (int)b - WX_OFFSET_MAX_HALF;
    return half < -WX_OFFSET_MAX_HALF ? -WX_OFFSET_MAX_HALF : half > 0 ? 0 : half;
}

uint8_t wx_offset_byte(int half) {
    half = half < -WX_OFFSET_MAX_HALF ? -WX_OFFSET_MAX_HALF : half > 0 ? 0 : half;
    return (uint8_t)(half + WX_OFFSET_MAX_HALF);
}

void wx_offset_text(char *out, size_t cap, int half) {
    if (half == 0) {
        snprintf(out, cap, "0 K");
    } else {
        snprintf(out, cap, "-%d.%d K", -half / 2, (-half % 2) ? 5 : 0);
    }
}

size_t wx_csv_header(char *out, size_t cap) {
    return (size_t)snprintf(out, cap, "time_utc,temperature_c,humidity_pct,pressure_hpa,gas_kohm\n");
}

static size_t csv_field(char *out, size_t cap, float v, char const *fmt) {
    if (isnan(v)) {
        return 0;
    }
    int n = snprintf(out, cap, fmt, (double)v);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

size_t wx_csv_line(char *out, size_t cap, int64_t unix_s, wx_sample_t const *s, float offset_k) {
    time_t    t = (time_t)unix_s;
    struct tm tm;
    if (cap < 96 || !gmtime_r(&t, &tm)) {
        return 0;
    }
    size_t len = strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
    static char const *const fmt[WX_QTY] = {"%.2f", "%.1f", "%.2f", "%.1f"};
    for (int q = 0; q < WX_QTY; q++) {
        out[len++] = ',';
        float v    = (q == WX_TEMP && !isnan(s->v[q])) ? s->v[q] + offset_k : s->v[q];
        len += csv_field(out + len, cap - len, v, fmt[q]);
    }
    out[len++] = '\n';
    out[len]   = '\0';
    return len;
}
