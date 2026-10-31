#include "time_fields.h"

#include "clock_local.h"

#include <stdio.h>

static int width(int field) {
    return field == TF_YEAR ? 4 : 2;
}

static void range(time_fields_t const *tf, int field, int *lo, int *hi) {
    switch (field) {
        case TF_YEAR: *lo = tf->year_min, *hi = tf->year_max; break;
        case TF_MONTH: *lo = 1, *hi = 12; break;
        case TF_DAY: *lo = 1, *hi = clock_days_in_month(tf->v[TF_YEAR], tf->v[TF_MONTH]); break;
        case TF_HOUR: *lo = 0, *hi = 23; break;
        default: *lo = 0, *hi = 59; break;
    }
}

static void clamp(time_fields_t *tf, int field) {
    int lo, hi;
    range(tf, field, &lo, &hi);
    int *v = &tf->v[field];
    *v     = *v < lo ? lo : *v > hi ? hi : *v;
}

// Year and month first: the day's range depends on them
static void clamp_all(time_fields_t *tf) {
    for (int f = 0; f < TF_COUNT; f++) {
        clamp(tf, f);
    }
}

void tf_init(time_fields_t *tf, int year, int month, int day, int hour, int minute, int year_min, int year_max) {
    *tf = (time_fields_t){.v = {year, month, day, hour, minute}, .year_min = year_min, .year_max = year_max};
    clamp_all(tf);
}

void tf_commit(time_fields_t *tf) {
    if (tf->typed == 0) {
        return;
    }
    // A part-typed year ("20") is no year in range: keep the old one. A single digit elsewhere is that number.
    if (tf->cursor == TF_YEAR && tf->typed < width(TF_YEAR)) {
        tf->v[TF_YEAR] = tf->before;
    }
    tf->typed = 0;
    clamp_all(tf);
}

void tf_step(time_fields_t *tf, int delta) {
    tf_commit(tf);
    int lo, hi;
    range(tf, tf->cursor, &lo, &hi);
    int span = hi - lo + 1;
    int v    = tf->v[tf->cursor] - lo + delta;
    tf->v[tf->cursor] = lo + ((v % span) + span) % span;
    clamp_all(tf); // a new month or year may shorten the month
}

void tf_move(time_fields_t *tf, int delta) {
    tf_commit(tf);
    int c      = tf->cursor + delta;
    tf->cursor = c < 0 ? 0 : c >= TF_COUNT ? TF_COUNT - 1 : c;
}

void tf_digit(time_fields_t *tf, int digit) {
    if (digit < 0 || digit > 9) {
        return;
    }
    int *v = &tf->v[tf->cursor];
    if (tf->typed == 0) {
        tf->before = *v;
        *v         = digit;
    } else {
        *v = *v * 10 + digit;
    }
    if (++tf->typed < width(tf->cursor)) {
        return;
    }
    tf->typed = 0;
    clamp_all(tf);
    if (tf->cursor < TF_COUNT - 1) {
        tf->cursor++;
    }
}

void tf_backspace(time_fields_t *tf) {
    if (tf->typed > 0) {
        tf->v[tf->cursor] = tf->before;
        tf->typed         = 0;
    }
}

void tf_field_text(time_fields_t const *tf, int field, char *out, size_t cap) {
    int w = width(field);
    if (field != tf->cursor || tf->typed == 0) {
        snprintf(out, cap, "%0*d", w, tf->v[field]);
        return;
    }
    snprintf(out, cap, "%0*d%.*s", tf->typed, tf->v[field], w - tf->typed, "____");
}
