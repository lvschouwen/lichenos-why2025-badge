// Trace path and path discovery (#53): see mesh_trace.h
#include "mesh_trace.h"

#include <stdio.h>
#include <string.h>

char const *mt_plan(uint8_t type, uint8_t path_len, uint8_t const *path, uint8_t const pub[MF_PUB_LEN],
                    uint8_t direct_hash_size, mt_plan_t *out) {
    memset(out, 0, sizeof(*out));
    out->target = -1;
    if (path_len == MT_PATH_UNKNOWN) {
        return "no path known: Discover path first";
    }
    unsigned count = path_len & 63, size = (unsigned)(path_len >> 6) + 1;
    bool     repeater = type == MT_ADV_REPEATER;
    if (count == 0) {
        if (!repeater) {
            return "a direct neighbour: no repeater on the path to trace";
        }
        size = direct_hash_size == 2 ? 2 : 1;
    }
    if (size > 2) {
        return "3-byte path hashes cannot be traced";
    }
    unsigned hops = repeater ? 2 * count + 1 : 2 * count - 1;
    if (hops > MF_PATH_MAX || hops * size > MF_TRACE_HASH_MAX) {
        return "the path is too long to trace";
    }
    out->flags    = size == 2 ? 1 : 0;
    out->hash_len = (uint8_t)(hops * size);
    uint8_t *h    = out->hashes;
    memcpy(h, path, count * size); // out along the path
    h += count * size;
    if (repeater) {
        out->target = (int8_t)count;
        memcpy(h, pub, size); // through the repeater itself
        h += size;
    }
    unsigned back = repeater ? count : count - 1; // a non-repeater: back from the last repeater
    for (unsigned i = back; i-- > 0;) {
        memcpy(h, path + i * size, size);
        h += size;
    }
    return NULL;
}

void mt_init(mt_t *t) {
    memset(t, 0, sizeof(*t));
    t->target = -1;
}

bool mt_busy(mt_t const *t) {
    return t->state == MT_QUEUED || t->state == MT_TAKEN || t->state == MT_SENT;
}

uint32_t mt_request(mt_t *t, mt_kind_t kind, uint8_t const prefix[MF_PREFIX], uint32_t tag, uint32_t now_ms,
                    char const **why) {
    if (mt_busy(t)) {
        *why = t->kind == MT_TRACE ? "a trace is still running" : "a path discovery is still running";
        return 0;
    }
    uint32_t seq = t->seq + 1 ? t->seq + 1 : 1;
    mt_init(t);
    t->seq      = seq;
    t->kind     = kind;
    t->state    = MT_QUEUED;
    t->tag      = tag;
    t->asked_ms = now_ms;
    memcpy(t->prefix, prefix, MF_PREFIX);
    return seq;
}

bool mt_take(mt_t *t, mt_kind_t *kind, uint8_t prefix[MF_PREFIX], uint32_t *tag, uint32_t now_ms) {
    if (t->state != MT_QUEUED) {
        return false;
    }
    t->state   = MT_TAKEN;
    t->sent_ms = now_ms;
    *kind      = t->kind;
    *tag       = t->tag;
    memcpy(prefix, t->prefix, MF_PREFIX);
    return true;
}

void mt_fail(mt_t *t, char const *why) {
    if (t->state == MT_QUEUED || t->state == MT_TAKEN) {
        t->state = MT_FAILED;
        t->why   = why;
    }
}

void mt_set_target(mt_t *t, int8_t target) {
    t->target = target;
}

void mt_on_reply(mt_t *t, mf_msg_t const *m, uint32_t now_ms) {
    if (t->state != MT_TAKEN) {
        return;
    }
    if (m->kind != MF_SENT) {
        t->state = MT_FAILED;
        t->err   = m->kind == MF_ERR ? m->err : 0;
        t->why   = m->kind != MF_ERR             ? "MeshCore did not take it"
                   : m->err == MF_ERR_NOT_FOUND  ? "contact not found"
                   : m->err == MF_ERR_TABLE_FULL ? "MeshCore is busy (no free packet)"
                                                 : "MeshCore refused it";
        return;
    }
    t->state   = MT_SENT;
    t->sent_ms = now_ms;
    t->flood   = m->flood;
    t->est_ms  = m->est_timeout;
    if (t->kind == MT_DISCOVER) {
        t->tag = m->ack; // MyMesh's request tag; the answer is matched by contact (MyMesh matched the tag)
    }
}

void mt_on_tx(mt_t *t, uint32_t tag, uint32_t now_ms) {
    if (t->kind == MT_TRACE && (t->state == MT_TAKEN || t->state == MT_SENT) && tag == t->tag && !t->on_air_ms) {
        t->on_air_ms = now_ms ? now_ms : 1;
    }
}

bool mt_on_trace(mt_t *t, mf_trace_t const *tr, uint32_t now_ms) {
    if (t->kind != MT_TRACE || t->state != MT_SENT || tr->tag != t->tag) {
        return false; // someone else's trace ending here, or ours after its time
    }
    t->trace   = *tr;
    t->state   = MT_DONE;
    t->done_ms = now_ms;
    return true;
}

bool mt_on_discovery(mt_t *t, mf_discovery_t const *d, uint32_t now_ms) {
    if (t->kind != MT_DISCOVER || t->state != MT_SENT || memcmp(d->prefix, t->prefix, MF_PREFIX) != 0) {
        return false;
    }
    t->disc    = *d;
    t->state   = MT_DONE;
    t->done_ms = now_ms;
    return true;
}

uint32_t mt_deadline_ms(uint32_t est_ms) {
    uint32_t d = est_ms > MT_CAP_MS ? MT_CAP_MS : est_ms + MT_GRACE_MS;
    return d < MT_FLOOR_MS ? MT_FLOOR_MS : d > MT_CAP_MS ? MT_CAP_MS : d;
}

void mt_poll(mt_t *t, uint32_t now_ms, mt_t *out) {
    if (t->state == MT_SENT && now_ms - t->sent_ms >= mt_deadline_ms(t->est_ms)) {
        t->state   = MT_TIMEOUT;
        t->done_ms = now_ms;
    } else if ((t->state == MT_QUEUED || t->state == MT_TAKEN) && now_ms - t->asked_ms >= MT_CAP_MS) {
        t->state = MT_FAILED; // the mesh task never got to it (stopped meanwhile)
        t->why   = "MeshCore did not get to it";
    }
    if (out) {
        *out = *t;
    }
}

uint32_t mt_round_trip_ms(mt_t const *t) {
    if (t->state != MT_DONE) {
        return 0;
    }
    uint32_t from = t->on_air_ms ? t->on_air_ms : t->sent_ms;
    return t->done_ms - from;
}

void mt_format_snr(int8_t q, char *out, size_t cap) {
    int      v    = q;
    char     sign = v < 0 ? '-' : 0;
    unsigned a    = (unsigned)(v < 0 ? -v : v);
    static char const *const frac[] = {"", ".25", ".5", ".75"};
    if (sign) {
        snprintf(out, cap, "-%u%s", a / 4, frac[a % 4]);
    } else {
        snprintf(out, cap, "%u%s", a / 4, frac[a % 4]);
    }
}

static size_t add(mt_line_t *out, size_t n, size_t max, char const *fmt, char const *a, char const *b) {
    if (n < max) {
        snprintf(out[n].text, sizeof(out[n].text), fmt, a, b);
        return n + 1;
    }
    return n;
}

static void hex_hash(uint8_t const *h, unsigned size, char *out, size_t cap) {
    if (size == 2) {
        snprintf(out, cap, "%02x%02x", h[0], h[1]);
    } else {
        snprintf(out, cap, "%02x", h[0]);
    }
}

// "N hops via a1,b2" (or "direct") for a packed path, cut to fit
static void route_text(uint8_t packed, uint8_t const *path, char *out, size_t cap) {
    unsigned count = packed & 63, size = (unsigned)(packed >> 6) + 1;
    if (count == 0) {
        snprintf(out, cap, "direct");
        return;
    }
    int at = snprintf(out, cap, "%u hop%s via ", count, count == 1 ? "" : "s");
    for (unsigned i = 0; i < count && at > 0 && (size_t)at + 6 < cap; i++) {
        char h[8];
        if (size <= 2) {
            hex_hash(path + i * size, size, h, sizeof(h));
        } else {
            snprintf(h, sizeof(h), "%02x%02x%02x", path[i * 3], path[i * 3 + 1], path[i * 3 + 2]);
        }
        at += snprintf(out + at, cap - (size_t)at, "%s%s", i ? "," : "", h);
    }
}

static size_t format_trace(mt_t const *t, mt_line_t *out, size_t max) {
    mf_trace_t const *tr   = &t->trace;
    unsigned          size = 1u << (tr->flags & 3);
    size_t            n    = 0;
    char              snr[12], hop[40];
    for (unsigned i = 0; i < tr->hops && size <= 2; i++) {
        char h[8];
        hex_hash(tr->hashes + i * size, size, h, sizeof(h));
        mt_format_snr(tr->snr_q[i], snr, sizeof(snr));
        snprintf(hop, sizeof(hop), "%u. %s%s", i + 1, h, (int)i == t->target ? " (contact)" : "");
        n = add(out, n, max, "%s  SNR %s dB", hop, snr);
    }
    mt_format_snr(tr->final_snr_q, snr, sizeof(snr));
    n = add(out, n, max, "Back here: SNR %s dB%s", snr, "");
    char rtt[16];
    snprintf(rtt, sizeof(rtt), "%lu", (unsigned long)mt_round_trip_ms(t));
    n = add(out, n, max, "Round trip: %s ms%s", rtt, "");
    if (t->target < 0) {
        n = add(out, n, max, "%s%s", "To the last repeater and back: only repeaters pass a trace on", "");
    }
    return n;
}

static size_t format_discovery(mt_t const *t, mt_line_t *out, size_t max) {
    char   route[MT_LINE_MAX - 13], rtt[16]; // "Path to it: " + route fits a line
    size_t n = 0;
    route_text(t->disc.out_len, t->disc.out_path, route, sizeof(route));
    n = add(out, n, max, "Path to it: %s%s", route, "");
    route_text(t->disc.in_len, t->disc.in_path, route, sizeof(route));
    n = add(out, n, max, "Its answer: %s%s", route, "");
    snprintf(rtt, sizeof(rtt), "%lu", (unsigned long)mt_round_trip_ms(t));
    n = add(out, n, max, "Round trip: %s ms%s", rtt, "");
    n = add(out, n, max, "%s%s", "Stored: messages now go along this path", "");
    return n;
}

size_t mt_format(mt_t const *t, mt_line_t *out, size_t max) {
    char secs[16];
    switch (t->state) {
    case MT_IDLE: return add(out, 0, max, "%s%s", "Nothing traced yet", "");
    case MT_QUEUED:
    case MT_TAKEN: return add(out, 0, max, "%s%s", "Sending...", "");
    case MT_SENT:
        snprintf(secs, sizeof(secs), "%lu", (unsigned long)(mt_deadline_ms(t->est_ms) / 1000));
        return add(out, 0, max, "Waiting for the answer (up to %s s)%s", secs, "");
    case MT_FAILED: return add(out, 0, max, "Not sent: %s%s", t->why ? t->why : "?", "");
    case MT_TIMEOUT:
        return add(out, 0, max, "%s%s",
                   t->kind == MT_TRACE ? "No answer: the trace did not come back in time"
                                       : "No answer: the contact did not reply in time",
                   "");
    case MT_DONE: break;
    }
    return t->kind == MT_TRACE ? format_trace(t, out, max) : format_discovery(t, out, max);
}
