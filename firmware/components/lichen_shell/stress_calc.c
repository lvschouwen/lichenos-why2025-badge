#include "stress_calc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t len_of(int n) {
    return n < 0 ? 0 : (size_t)n;
}

static char const *skip_spaces(char const *s) {
    while (*s == ' ') {
        s++;
    }
    return s;
}

// The word ends at the end of the text or at trailing spaces only
static bool word_is(char const *s, char const *word) {
    size_t n = strlen(word);
    return strncmp(s, word, n) == 0 && *skip_spaces(s + n) == '\0';
}

stress_cmd_t stress_parse(char const *args, unsigned *minutes) {
    char const *s = skip_spaces(args ? args : "");
    *minutes      = STRESS_MIN_DEFAULT;
    if (*s == '\0') {
        return STRESS_CMD_START;
    }
    if (word_is(s, "stop")) {
        return STRESS_CMD_STOP;
    }
    if (word_is(s, "status")) {
        return STRESS_CMD_STATUS;
    }
    char         *end = NULL;
    unsigned long n   = strtoul(s, &end, 10);
    if (end == s || *skip_spaces(end) != '\0' || *s == '-' || *s == '+' || n < 1 || n > STRESS_MIN_MAX) {
        return STRESS_CMD_BAD;
    }
    *minutes = (unsigned)n;
    return STRESS_CMD_START;
}

// Indexed by stress_reason_t; one string each, so the log lines and the screen share them
static char const *const REASONS[STRESS_REASONS] = {
    "ok", "mismatch", "dma_bad", "heap", "alloc", "lockcheck", "latency", "dma_floor", "stack", "hung",
    "esc", "stop", "poweroff", "update", "trial", "hot", "c6",
};

char const *stress_reason_name(stress_reason_t r) {
    return (unsigned)r < STRESS_REASONS ? REASONS[r] : "?";
}

bool stress_failed(stress_reason_t r) {
    return r > STRESS_NONE && r < STRESS_FAILURES;
}

bool stress_stack_fell_low(uint32_t free_now, uint32_t free_at_start) {
    if (free_now >= STRESS_LIMIT_STACK || free_now >= free_at_start) {
        return false;
    }
    return free_at_start >= STRESS_LIMIT_STACK || free_at_start - free_now > STRESS_STACK_MARGIN;
}

uint32_t stress_since(uint32_t now, uint32_t base) {
    return now >= base ? now - base : now;
}

stress_reason_t stress_outside(stress_outside_t const *in) {
    if (in->asked != STRESS_NONE) {
        return in->asked;
    }
    bool const      hit[] = {in->poweroff, in->update, in->trial, in->hot, in->c6_trouble};
    stress_reason_t why[] = {STRESS_POWEROFF, STRESS_UPDATE, STRESS_TRIAL, STRESS_HOT, STRESS_C6};
    for (size_t i = 0; i < sizeof(hit) / sizeof(hit[0]); i++) {
        if (hit[i]) {
            return why[i];
        }
    }
    return STRESS_NONE;
}

stress_reason_t stress_judge(stress_obs_t const *o) {
    if (o->mismatches) {
        return STRESS_MISMATCH;
    }
    if (o->dma_bad) {
        return STRESS_DMA_BAD;
    }
    if (!o->heap_ok) {
        return STRESS_HEAP;
    }
    if (o->alloc_fails) {
        return STRESS_ALLOC;
    }
    if (o->lock_hits) {
        return STRESS_LOCKCHECK;
    }
    if (o->s.lat_ms > STRESS_LIMIT_LAT_MS) {
        return STRESS_LATENCY;
    }
    if (o->s.dma_free < STRESS_LIMIT_DMA && !o->in_dma_round) {
        return STRESS_DMA_FLOOR;
    }
    if (o->stack_low < STRESS_LIMIT_STACK) {
        return STRESS_STACK;
    }
    return STRESS_NONE;
}

void stress_run_reset(stress_run_t *run) {
    memset(run, 0, sizeof(*run));
}

static void lower(uint32_t *lo, uint32_t v) {
    *lo = v < *lo ? v : *lo;
}

static void higher(uint32_t *hi, uint32_t v) {
    *hi = v > *hi ? v : *hi;
}

void stress_track(stress_run_t *run, stress_sample_t const *s) {
    if (!run->have) {
        run->lo   = *s;
        run->have = true;
        return;
    }
    stress_sample_t *lo = &run->lo;
    lower(&lo->dma_free, s->dma_free);
    lower(&lo->dma_min, s->dma_min);
    lower(&lo->dma_largest, s->dma_largest);
    lower(&lo->int_free, s->int_free);
    lower(&lo->int_min, s->int_min);
    lower(&lo->int_largest, s->int_largest);
    lower(&lo->psram_free, s->psram_free);
    higher(&lo->pool_peak, s->pool_peak);
    higher(&lo->pool_drop, s->pool_drop);
    higher(&lo->lat_ms, s->lat_ms);
}

bool stress_latch(stress_run_t *run, stress_reason_t r, uint32_t t_s) {
    if (r == STRESS_NONE || run->reason != STRESS_NONE) {
        return false;
    }
    run->reason = r;
    run->stop_s = t_s;
    return true;
}

char const *stress_refusal(stress_gate_in_t const *in) {
    if (in->running) {
        return "a stress run is already on";
    }
    if (in->update || in->poweroff) {
        return "an update or power off is under way";
    }
    if (in->psram_trial) {
        return "the 120 MHz PSRAM trial is running";
    }
    if (in->c6_trouble) {
        return "the C6 link is not up";
    }
    if (in->hot) {
        return "the badge is hot (thermal warning)";
    }
    if (in->dma_free < STRESS_START_DMA || in->psram_free < STRESS_START_PSRAM) {
        return "low memory";
    }
    return NULL;
}

size_t stress_fmt_start(char *out, size_t cap, unsigned minutes, bool sd, bool fs) {
    return len_of(snprintf(out, cap, "start %u min workers dma%s%s,ui,lat%s%s", minutes, sd ? ",sd" : "",
                           fs ? ",fs" : "", sd ? "" : " sd skipped: no card", fs ? "" : " fs skipped: no drive"));
}

// The figures of a sample or of the run's record, shared by the sample and the min line
static size_t fmt_figures(char *out, size_t cap, stress_sample_t const *s) {
    return len_of(snprintf(out, cap,
                           "dma free=%u min=%u largest=%u int free=%u min=%u largest=%u psram=%u pool peak=%u "
                           "drop=%u lat=%ums",
                           (unsigned)s->dma_free, (unsigned)s->dma_min, (unsigned)s->dma_largest,
                           (unsigned)s->int_free, (unsigned)s->int_min, (unsigned)s->int_largest,
                           (unsigned)s->psram_free, (unsigned)s->pool_peak, (unsigned)s->pool_drop,
                           (unsigned)s->lat_ms));
}

// prefix, the figures, suffix: one snprintf-rules line however small cap is
static size_t fmt_wrapped(char *out, size_t cap, char const *prefix, stress_sample_t const *s, char const *suffix) {
    char   figures[256]; // ten 32-bit figures at their widest fit: the length returned is the whole line's
    size_t n = fmt_figures(figures, sizeof(figures), s);
    if (n >= sizeof(figures)) {
        n = sizeof(figures) - 1;
    }
    return len_of(snprintf(out, cap, "%s%.*s%s", prefix, (int)n, figures, suffix));
}

size_t stress_fmt_sample(char *out, size_t cap, uint32_t t_s, stress_sample_t const *s, stress_reason_t r) {
    char prefix[24], suffix[16];
    snprintf(prefix, sizeof(prefix), "t=%us ", (unsigned)t_s);
    snprintf(suffix, sizeof(suffix), " %s", stress_reason_name(r));
    return fmt_wrapped(out, cap, prefix, s, suffix);
}

size_t stress_fmt_min(char *out, size_t cap, stress_sample_t const *lo) {
    return fmt_wrapped(out, cap, "min ", lo, "");
}

size_t stress_fmt_stop(char *out, size_t cap, stress_reason_t r, uint32_t t_s) {
    return len_of(snprintf(out, cap, "STOP %s at %us", stress_reason_name(r), (unsigned)t_s));
}

size_t stress_fmt_done(char *out, size_t cap, uint32_t t_s, bool pass) {
    return len_of(snprintf(out, cap, "done %us %s", (unsigned)t_s, pass ? "PASS" : "FAIL"));
}

size_t stress_fmt_stack(char *out, size_t cap, char const *task, uint32_t free, uint32_t size) {
    if (size == 0) {
        return len_of(snprintf(out, cap, "stack %.15s %u/?", task, (unsigned)free));
    }
    return len_of(snprintf(out, cap, "stack %.15s %u/%u", task, (unsigned)free, (unsigned)size));
}

static char const *const COUNTS[STRESS_C_COUNT] = {
    "dma_bad", "dma_busy", "mismatch", "alloc", "heap", "lock", "sd_io", "fs_io",
    "dma", "copies", "sd", "sd_kb", "fs", "frames",
};

uint32_t stress_errors(stress_counts_t const *c) {
    uint32_t n = 0;
    for (int i = 0; i < STRESS_C_WORK; i++) {
        n += i == STRESS_C_DMA_BUSY ? 0 : c->n[i];
    }
    return n;
}

// One "name=value" after the other, from the table; snprintf rules for the whole line
size_t stress_fmt_counts(char *out, size_t cap, stress_counts_t const *c, bool work) {
    size_t n = len_of(snprintf(out, cap, work ? "work" : "errors"));
    for (int i = work ? STRESS_C_WORK : 0; i < (work ? STRESS_C_COUNT : STRESS_C_WORK); i++) {
        n += len_of(snprintf(n < cap ? out + n : NULL, n < cap ? cap - n : 0, " %s=%u", COUNTS[i], (unsigned)c->n[i]));
    }
    return n;
}

size_t stress_fmt_clock(char *out, size_t cap, uint32_t t_s, uint32_t total_s) {
    return len_of(snprintf(out, cap, "%02u:%02u / %02u:%02u", (unsigned)(t_s / 60), (unsigned)(t_s % 60),
                           (unsigned)(total_s / 60), (unsigned)(total_s % 60)));
}

size_t stress_fmt_workers(char *out, size_t cap, stress_counts_t const *c, unsigned failed, unsigned skipped, uint32_t lat_ms) {
    static char const *const name[] = {"DMA", "SD", "Storage", "UI", "Latency"};
    static char const *const unit[] = {"rounds", "files", "files", "frames", "ms"};
    uint32_t const work[] = {c->n[STRESS_C_DMA], c->n[STRESS_C_SD], c->n[STRESS_C_FS], c->n[STRESS_C_FRAMES], lat_ms};
    size_t         n      = 0;
    for (unsigned i = 0; i < 5; i++) {
        char       *at   = n < cap ? out + n : NULL;
        size_t      room = n < cap ? cap - n : 0;
        char const *nl   = i ? "\n" : "";
        if (skipped & (1u << i)) {
            n += len_of(snprintf(at, room, "%s%s: skipped, no %s", nl, name[i], i == 1 ? "card" : "drive"));
        } else {
            n += len_of(snprintf(at, room, "%s%s: %s, %s%u %s", nl, name[i], failed & (1u << i) ? "FAIL" : "ok",
                                 i == 4 ? "worst " : "", (unsigned)work[i], unit[i]));
        }
    }
    return n;
}
