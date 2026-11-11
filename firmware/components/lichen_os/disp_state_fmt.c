#include "disp_state_fmt.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Table driven so the line costs no per-field buffers (it runs on a small task stack): each row is the literal text
// before a value, how to read the value, and where it sits in ds_info_t.
typedef enum { K_TOKEN, K_MHZ, K_INT, K_I64, K_YESNO, K_UPDOWN } kind_t;

typedef struct {
    char const *before;
    kind_t      kind;
    size_t      off;
} row_t;

#define ROW(before_, kind_, field_) {before_, kind_, offsetof(ds_info_t, field_)}

static row_t const rows[] = {
    ROW("disp state: boot ", K_TOKEN, reset),
    ROW(" ", K_TOKEN, fw),
    ROW(" | psram ", K_MHZ, psram_clk_raw),
    ROW(" want ", K_INT, psram_want_mhz),
    ROW(" failed ", K_YESNO, failed),
    ROW(" arms ", K_INT, arms),
    ROW(" fall ", K_TOKEN, fall_raw),
    ROW(" heapmax ", K_I64, heap_max_us),
    ROW("us | pclk ", K_TOKEN, pclk_raw),
    ROW(" refresh ", K_TOKEN, refresh_raw),
    ROW(" | bl ", K_INT, bl_req),
    ROW("/", K_TOKEN, bl_raw),
    ROW(" kbd ", K_INT, kbd_req),
    ROW("/", K_TOKEN, kbd_raw),
    ROW(" boot ", K_TOKEN, bl_boot_raw),
    ROW(" idle ", K_TOKEN, bl_idle_raw),
    ROW(" override ", K_TOKEN, bl_override_raw),
    ROW(" freq ", K_TOKEN, bl_freq_raw),
    ROW(" | c6 ", K_UPDOWN, c6_up),
    ROW(" restarts ", K_I64, c6_restarts),
    ROW(" | uptime ", K_I64, uptime_s),
};

typedef struct {
    char  *out;
    size_t cap, len;
} sink_t;

static void put(sink_t *s, char const *text, size_t n) {
    for (size_t i = 0; i < n && s->len + 1 < s->cap; i++) {
        s->out[s->len++] = text[i];
    }
}

static void put_str(sink_t *s, char const *text) {
    put(s, text, strlen(text));
}

// The token before the first space of a registry text; "?" when none or not read ("? (not read yet)")
static void put_token(sink_t *s, char const *raw) {
    size_t n = 0;
    while (raw && raw[n] && raw[n] != ' ') {
        n++;
    }
    if (n == 0 || raw[0] == '?') {
        put_str(s, "?");
    } else {
        put(s, raw, n);
    }
}

// psram.clk is in Hz; the line says MHz
static void put_mhz(sink_t *s, char const *raw) {
    char *end = NULL;
    long  hz  = raw ? strtol(raw, &end, 10) : 0;
    if (!raw || end == raw || (*end && *end != ' ')) {
        put_str(s, "?");
        return;
    }
    char num[24];
    snprintf(num, sizeof(num), "%ld", (hz + 500000) / 1000000);
    put_str(s, num);
}

static void put_num(sink_t *s, int64_t v) {
    char num[24];
    if (v < 0) {
        put_str(s, "?");
    } else {
        snprintf(num, sizeof(num), "%lld", (long long)v);
        put_str(s, num);
    }
}

static void put_value(sink_t *s, row_t const *r, ds_info_t const *in) {
    void const *p = (char const *)in + r->off;
    switch (r->kind) {
        case K_TOKEN: put_token(s, *(char const *const *)p); break;
        case K_MHZ: put_mhz(s, *(char const *const *)p); break;
        case K_INT: put_num(s, *(int const *)p); break;
        case K_I64: put_num(s, *(int64_t const *)p); break;
        case K_YESNO: put_str(s, *(int const *)p < 0 ? "?" : *(int const *)p ? "yes" : "no"); break;
        case K_UPDOWN: put_str(s, *(int const *)p < 0 ? "?" : *(int const *)p ? "up" : "down"); break;
    }
}

size_t ds_format(ds_info_t const *in, char *out, size_t cap) {
    if (cap == 0) {
        return 0;
    }
    sink_t s = {out, cap, 0};
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        put_str(&s, rows[i].before);
        if (rows[i].off == offsetof(ds_info_t, fw) && !(in->fw && in->fw[0])) {
            put_str(&s, "fw-?"); // the one value with a longer missing text
        } else {
            put_value(&s, &rows[i], in);
        }
    }
    out[s.len] = '\0';
    return s.len;
}
