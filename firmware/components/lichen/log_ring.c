#include "log_ring.h"

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

static uint32_t get32(log_ring_t const *r, size_t at) {
    uint8_t b[4];
    copy_out(r, at, b, 4);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
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

static void commit(log_ring_t *r) {
    size_t need = LOG_RING_HEADER + r->pend_len;
    if (r->pend_len > 0 && need <= r->cap) {
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

size_t log_ring_read(log_ring_t const *r, uint32_t since, char *out, size_t cap, uint32_t *next, uint32_t *missed) {
    *missed = 0;
    if (since == 0 || since > r->next_seq) {
        since = r->first_seq;
    } else if (since < r->first_seq) {
        *missed = r->first_seq - since;
        since   = r->first_seq;
    }
    *next = since;

    size_t at = r->head;
    for (uint32_t seq = r->first_seq; seq < since; seq++) {
        at = (at + LOG_RING_HEADER + text_len(r, at)) % r->cap;
    }
    size_t written = 0;
    for (uint32_t seq = since; seq < r->next_seq; seq++) {
        size_t n = text_len(r, at);
        if (cap - written < LOG_RING_OUT_LINE) {
            break;
        }
        uint32_t ms = get32(r, (at + 4) % r->cap);
        int      h  = snprintf(out + written, cap - written, "%u %u.%03u ", (unsigned)seq, (unsigned)(ms / 1000),
                               (unsigned)(ms % 1000));
        if (h < 0 || (size_t)h + n + 1 >= cap - written) {
            break;
        }
        copy_out(r, (at + LOG_RING_HEADER) % r->cap, out + written + h, n);
        written            += (size_t)h + n;
        out[written++]      = '\n';
        at                  = (at + LOG_RING_HEADER + n) % r->cap;
        *next               = seq + 1;
    }
    return written;
}
