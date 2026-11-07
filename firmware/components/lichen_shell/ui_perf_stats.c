#include "ui_perf_stats.h"

#include <stdio.h>

void ui_perf_reset(ui_perf_t *p, uint32_t now_ms) {
    *p          = (ui_perf_t){0};
    p->since_ms = now_ms;
}

void perf_acc_add(perf_acc_t *a, uint32_t us) {
    a->n++;
    a->sum_us += us;
    if (us > a->max_us) {
        a->max_us = us;
    }
}

uint32_t perf_acc_avg_us(perf_acc_t const *a) {
    return a->n ? (uint32_t)(a->sum_us / a->n) : 0;
}

// tenths of a millisecond, for "%u.%u"
static unsigned tenths(uint32_t us) {
    return (unsigned)((us + 50) / 100);
}

size_t ui_perf_format(char *out, size_t cap, ui_perf_t const *p, uint32_t now_ms) {
    uint32_t span_ms = now_ms - p->since_ms;
    if (p->render.n == 0) {
        int n = snprintf(out, cap, "ui perf: no frames in %u.%u s", (unsigned)(span_ms / 1000), (unsigned)(span_ms % 1000 / 100));
        return n < 0 ? 0 : (size_t)n;
    }
    unsigned fps10 = span_ms ? (unsigned)(((uint64_t)p->render.n * 10000 + span_ms / 2) / span_ms) : 0; // frames per second, tenths
    unsigned ra = tenths(perf_acc_avg_us(&p->render)), rm = tenths(p->render.max_us);
    unsigned fa = tenths(perf_acc_avg_us(&p->flush)), fm = tenths(p->flush.max_us);
    unsigned wa = tenths(perf_acc_avg_us(&p->wait)), wm = tenths(p->wait.max_us);
    int      n  = snprintf(out, cap,
                           "ui perf: %u frames in %u.%u s (%u.%u/s), render avg %u.%u max %u.%u ms, %u flushes avg %u.%u max %u.%u ms, "
                           "flush wait avg %u.%u max %u.%u ms",
                           (unsigned)p->render.n, (unsigned)(span_ms / 1000), (unsigned)(span_ms % 1000 / 100), fps10 / 10, fps10 % 10,
                           ra / 10, ra % 10, rm / 10, rm % 10, (unsigned)p->flush.n, fa / 10, fa % 10, fm / 10, fm % 10, wa / 10,
                           wa % 10, wm / 10, wm % 10);
    return n < 0 ? 0 : (size_t)n;
}

void perf_hist_add(perf_hist_t *h, uint32_t us) {
    uint32_t ms = us / 1000;
    unsigned i  = ms > PERF_HIST_MAX_MS ? PERF_HIST_MAX_MS : ms;
    if (h->bucket[i] < UINT16_MAX) {
        h->bucket[i]++;
    }
    h->n++;
}

uint32_t perf_hist_p95_ms(perf_hist_t const *h) {
    if (h->n == 0) {
        return 0;
    }
    uint64_t need = ((uint64_t)h->n * 95 + 99) / 100; // samples at or below the 95th percentile, rounded up
    uint64_t seen = 0;
    for (unsigned i = 0; i <= PERF_HIST_MAX_MS; i++) {
        seen += h->bucket[i];
        if (seen >= need) {
            return i + 1;
        }
    }
    return PERF_HIST_MAX_MS + 1; // a saturated bucket lost counts: report "slow"
}

size_t ui_perf_format_latency(char *out, size_t cap, ui_perf_lat_t const *p, uint32_t first_frame_ms) {
    char first[32] = "none yet";
    if (first_frame_ms) {
        snprintf(first, sizeof(first), "%u ms", (unsigned)first_frame_ms);
    }
    int n;
    if (p->key.n == 0) {
        n = snprintf(out, cap, "ui perf latency: render p95 %u ms, key-to-flush none, first frame at %s",
                     (unsigned)perf_hist_p95_ms(&p->render_hist), first);
    } else {
        unsigned ka = tenths(perf_acc_avg_us(&p->key)), km = tenths(p->key.max_us);
        n = snprintf(out, cap, "ui perf latency: render p95 %u ms, key-to-flush %u keys avg %u.%u max %u.%u p95 %u ms, first frame at %s",
                     (unsigned)perf_hist_p95_ms(&p->render_hist), (unsigned)p->key.n, ka / 10, ka % 10, km / 10, km % 10,
                     (unsigned)perf_hist_p95_ms(&p->key_hist), first);
    }
    return n < 0 ? 0 : (size_t)n;
}
