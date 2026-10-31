#include "log_ring.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void copy_in(log_ring_t *r, size_t at, void const *src, size_t n) {
    size_t first = r->cap - at < n ? r->cap - at : n;
    memcpy(r->buf + at, src, first);
    memcpy(r->buf, (uint8_t const *)src + first, n - first);
}

static void copy_out(log_ring_t const *r, size_t at, void *dst, size_t n) {
    size_t first = r->cap - at < n ? r->cap - at : n;
    memcpy(dst, r->buf + at, first);
    memcpy((uint8_t *)dst + first, r->buf, n - first);
}

static size_t text_len(log_ring_t const *r, size_t at) {
    uint8_t b[2];
    copy_out(r, (at + 8) % r->cap, b, 2);
    return (size_t)b[0] | (size_t)b[1] << 8;
}

void log_ring_init(log_ring_t *r, uint8_t *buf, size_t cap) {
    memset(r, 0, sizeof(*r));
    r->buf       = buf;
    r->cap       = cap;
    r->first_seq = 1;
    r->next_seq  = 1;
}

static void evict_oldest(log_ring_t *r) {
    size_t size = LOG_RING_HEADER + text_len(r, r->head);
    r->head     = (r->head + size) % r->cap;
    r->used    -= size;
    r->first_seq++;
}

// Replaces what a terminal could act on with '?', byte for byte: C0 controls (but tab), DEL, and anything that is not
// well-formed UTF-8 (stray continuation bytes, overlong forms, surrogates, > U+10FFFF, C1 controls U+0080..U+009F).
static void sanitise(char *s, size_t len) {
    uint8_t *p = (uint8_t *)s;
    size_t   i = 0;
    while (i < len) {
        uint8_t c = p[i];
        if (c < 0x80) {
            if ((c < 0x20 && c != '\t') || c == 0x7f) {
                p[i] = '?';
            }
            i++;
            continue;
        }
        size_t  need = 0;
        uint8_t lo = 0x80, hi = 0xbf; // allowed range of the first continuation byte
        if (c >= 0xc2 && c <= 0xdf) {
            need = 1;
            lo   = c == 0xc2 ? 0xa0 : 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            need = 2;
            lo   = c == 0xe0 ? 0xa0 : 0x80;
            hi   = c == 0xed ? 0x9f : 0xbf;
        } else if (c >= 0xf0 && c <= 0xf4) {
            need = 3;
            lo   = c == 0xf0 ? 0x90 : 0x80;
            hi   = c == 0xf4 ? 0x8f : 0xbf;
        }
        bool ok = need > 0 && len - i > need && p[i + 1] >= lo && p[i + 1] <= hi;
        for (size_t k = 2; ok && k <= need; k++) {
            ok = p[i + k] >= 0x80 && p[i + k] <= 0xbf;
        }
        if (ok) {
            i += need + 1;
        } else {
            p[i++] = '?';
        }
    }
}

static void commit(log_ring_t *r) {
    size_t need = LOG_RING_HEADER + r->pend_len;
    if (r->pend_len > 0 && need <= r->cap) {
        sanitise(r->pend, r->pend_len);
        while (r->cap - r->used < need) {
            evict_oldest(r);
        }
        uint8_t hdr[LOG_RING_HEADER] = {
            (uint8_t)r->next_seq, (uint8_t)(r->next_seq >> 8), (uint8_t)(r->next_seq >> 16), (uint8_t)(r->next_seq >> 24),
            (uint8_t)r->pend_ms,  (uint8_t)(r->pend_ms >> 8),  (uint8_t)(r->pend_ms >> 16),  (uint8_t)(r->pend_ms >> 24),
            (uint8_t)r->pend_len, (uint8_t)(r->pend_len >> 8),
        };
        size_t at = (r->head + r->used) % r->cap;
        copy_in(r, at, hdr, sizeof(hdr));
        copy_in(r, (at + sizeof(hdr)) % r->cap, r->pend, r->pend_len);
        if (r->next_seq % LOG_RING_IDX_STEP == 0) {
            size_t slot       = (r->next_seq / LOG_RING_IDX_STEP) % LOG_RING_IDX_SLOTS;
            r->idx_seq[slot]  = r->next_seq;
            r->idx_at[slot]   = (uint32_t)at;
        }
        r->used += need;
        r->next_seq++;
        if (r->used == need) {
            r->first_seq = r->next_seq - 1; // the ring was empty
        }
    }
    r->pend_len = 0;
}

void log_ring_write(log_ring_t *r, uint32_t ms, char const *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\n') {
            commit(r);
        } else if (c != '\r') {
            if (r->pend_len == LOG_RING_LINE_MAX) {
                commit(r);
            }
            if (r->pend_len == 0) {
                r->pend_ms = ms;
            }
            r->pend[r->pend_len++] = c;
        }
    }
}

// Offset of the record with sequence number seq (first_seq <= seq <= next_seq): from the nearest indexed record at or
// before it, else from the oldest. At most LOG_RING_IDX_STEP - 1 records are walked while the index is intact.
static size_t seek(log_ring_t const *r, uint32_t seq) {
    uint32_t base = seq - seq % LOG_RING_IDX_STEP;
    size_t   slot = (base / LOG_RING_IDX_STEP) % LOG_RING_IDX_SLOTS;
    uint32_t cur  = r->first_seq;
    size_t   at   = r->head;
    if (base >= r->first_seq && r->idx_seq[slot] == base) {
        cur = base;
        at  = r->idx_at[slot];
    }
    for (; cur < seq; cur++) {
        at = (at + LOG_RING_HEADER + text_len(r, at)) % r->cap;
    }
    return at;
}

size_t log_ring_copy(log_ring_t const *r, uint32_t since, uint8_t *raw, size_t raw_cap, size_t out_budget,
                     uint32_t *next, uint32_t *missed) {
    *missed = 0;
    if (since == 0 || since > r->next_seq) {
        since = r->first_seq;
    } else if (since < r->first_seq) {
        *missed = r->first_seq - since;
        since   = r->first_seq;
    }
    *next = since;

    size_t at = seek(r, since);
    size_t len = 0, fmt = 0;
    for (uint32_t seq = since; seq < r->next_seq; seq++) {
        size_t n   = text_len(r, at);
        size_t rec = LOG_RING_HEADER + n;
        if (raw_cap - len < rec || out_budget - fmt < n + LOG_RING_ROW_EXTRA) {
            break;
        }
        copy_out(r, at, raw + len, rec);
        len  += rec;
        fmt  += n + LOG_RING_ROW_EXTRA;
        at    = (at + rec) % r->cap;
        *next = seq + 1;
    }
    return len;
}

static uint32_t le32(uint8_t const *b) {
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

size_t log_ring_format(uint8_t const *raw, size_t raw_len, char *out, size_t cap) {
    size_t written = 0;
    for (size_t at = 0; at + LOG_RING_HEADER <= raw_len;) {
        size_t n = (size_t)raw[at + 8] | (size_t)raw[at + 9] << 8;
        if (at + LOG_RING_HEADER + n > raw_len) {
            break;
        }
        uint32_t ms = le32(raw + at + 4);
        int      h  = snprintf(out + written, cap - written, "%u %u.%03u ", (unsigned)le32(raw + at),
                               (unsigned)(ms / 1000), (unsigned)(ms % 1000));
        if (h < 0 || (size_t)h + n + 1 >= cap - written) {
            break;
        }
        memcpy(out + written + h, raw + at + LOG_RING_HEADER, n);
        written        += (size_t)h + n;
        out[written++]  = '\n';
        at             += LOG_RING_HEADER + n;
    }
    return written;
}
