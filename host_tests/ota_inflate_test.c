// Host test: the rules a compressed upload body must keep (#298, ota_inflate.c), with real zlib streams decoded by the
// host's zlib in place of the badge's ROM tinfl, and with scripted decoders that misbehave on purpose. Runs under
// ASan+UBSan: every sink is exactly as large as the cap, so a byte written past it is reported.
#include "mutate.h"
#include "ota_inflate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// Test allocations: out of memory ends the test instead of being a case of its own
static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        printf("out of memory\n");
        exit(2);
    }
    return p;
}

// --- zlib as the decoder: small output spans, like tinfl's window wrapping ---

typedef struct {
    z_stream zs;
    uint8_t  out[61];
} zdec_t;

static ota_inflate_status_t z_step(void *dec, uint8_t const *in, size_t *in_len, uint8_t const **out, size_t *out_len) {
    zdec_t *d        = dec;
    d->zs.next_in    = (Bytef *)(uintptr_t)in;
    d->zs.avail_in   = (uInt)*in_len;
    d->zs.next_out   = d->out;
    d->zs.avail_out  = sizeof(d->out);
    int rc           = inflate(&d->zs, Z_NO_FLUSH);
    *in_len         -= d->zs.avail_in;
    *out             = d->out;
    *out_len         = sizeof(d->out) - d->zs.avail_out;
    if (rc == Z_STREAM_END) {
        return OTA_INFLATE_DONE;
    }
    if (rc == Z_OK || rc == Z_BUF_ERROR) {
        return d->zs.avail_out == 0 ? OTA_INFLATE_HAS_OUTPUT : OTA_INFLATE_NEEDS_INPUT;
    }
    return OTA_INFLATE_ERROR;
}

static void z_open(zdec_t *d) {
    memset(d, 0, sizeof(*d));
    CHECK(inflateInit(&d->zs) == Z_OK);
}

// A sink of exactly cap bytes on the heap
typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    size_t   calls;
    size_t   refuse_after; // refuse the call after this many (0: never)
} sink_t;

static bool sink_put(void *ctx, uint8_t const *data, size_t len) {
    sink_t *s = ctx;
    s->calls++;
    if (s->refuse_after && s->calls > s->refuse_after) {
        return false;
    }
    if (len > s->cap - s->len) {
        printf("sink overrun: %zu bytes into %zu left\n", len, s->cap - s->len);
        failures++;
        return false;
    }
    memcpy(s->buf + s->len, data, len);
    s->len += len;
    return true;
}

static sink_t sink_new(size_t cap) {
    sink_t s = {.buf = xmalloc(cap), .cap = cap};
    return s;
}

static size_t deflate_into(uint8_t const *src, size_t len, uint8_t **out) {
    uLongf n = compressBound((uLong)len);
    *out     = xmalloc(n);
    CHECK(compress2(*out, &n, src, (uLong)len, 9) == Z_OK);
    return (size_t)n;
}

// Decode stream (len bytes) in chunks of at most step bytes, into a sink of cap bytes; true when it fed cleanly
static bool decode(uint8_t const *stream, size_t len, size_t step, size_t cap, ota_inflate_t *z, sink_t *s) {
    static zdec_t d;
    z_open(&d);
    ota_inflate_begin(z, z_step, &d, cap);
    *s      = sink_new(cap);
    bool ok = true;
    for (size_t at = 0; ok && at < len; at += step) {
        ok = ota_inflate_feed(z, stream + at, len - at < step ? len - at : step, sink_put, s);
    }
    inflateEnd(&d.zs);
    return ok;
}

static void fill(uint8_t *buf, size_t len, uint32_t seed) {
    mut_rng_t r;
    mut_seed(&r, seed);
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(i % 97 < 60 ? i / 7 : mut_next(&r)); // half text-like runs, half noise
    }
}

#define PIECE 131072

// A piece decodes exactly, in any chunking (the socket hands over 1..4096 bytes at a time)
static void test_round_trip(void) {
    uint8_t *raw = xmalloc(PIECE);
    fill(raw, PIECE, 298);
    uint8_t *zs;
    size_t   zn = deflate_into(raw, PIECE, &zs);
    CHECK(zn < PIECE);
    static size_t const steps[] = {1, 7, 1460, 4096, 1 << 20};
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        ota_inflate_t z;
        sink_t        s;
        CHECK(decode(zs, zn, steps[i], PIECE, &z, &s));
        CHECK(ota_inflate_complete(&z, PIECE) && s.len == PIECE && memcmp(s.buf, raw, PIECE) == 0);
        CHECK(!ota_inflate_complete(&z, PIECE - 1) && !ota_inflate_complete(&z, PIECE + 1));
        free(s.buf);
    }
    free(zs);
    free(raw);
}

// An inflate bomb: 4 MB of zeros in about 4 KB must stop at the cap, before a byte past it reaches the sink
static void test_bomb(void) {
    size_t   big  = 4u << 20;
    uint8_t *zero = xmalloc(big);
    memset(zero, 0, big);
    uint8_t *zs;
    size_t   zn = deflate_into(zero, big, &zs);
    CHECK(zn < 8192);
    ota_inflate_t z;
    sink_t        s;
    CHECK(!decode(zs, zn, 4096, PIECE, &z, &s));
    CHECK(z.failed && s.len <= PIECE && z.produced <= PIECE && !ota_inflate_complete(&z, PIECE));
    free(s.buf);
    // Exactly the cap is fine; one byte under it is refused
    uint8_t *zs2;
    size_t   zn2 = deflate_into(zero, PIECE, &zs2);
    CHECK(decode(zs2, zn2, 4096, PIECE, &z, &s) && ota_inflate_complete(&z, PIECE));
    free(s.buf);
    CHECK(!decode(zs2, zn2, 4096, PIECE - 1, &z, &s) && s.len <= PIECE - 1);
    free(s.buf);
    free(zs2);
    free(zs);
    free(zero);
}

// Nothing may follow the stream, in the same read or a later one; a truncated stream never completes
static void test_trailing_and_truncated(void) {
    uint8_t raw[3000];
    fill(raw, sizeof(raw), 7);
    uint8_t *zs;
    size_t   zn = deflate_into(raw, sizeof(raw), &zs);
    uint8_t *tail = xmalloc(zn + 1);
    memcpy(tail, zs, zn);
    tail[zn] = 'x';
    ota_inflate_t z;
    sink_t        s;
    CHECK(!decode(tail, zn + 1, 1 << 20, sizeof(raw), &z, &s) && !ota_inflate_complete(&z, sizeof(raw)));
    free(s.buf);
    CHECK(!decode(tail, zn + 1, zn, sizeof(raw), &z, &s)); // the stream, then the stray byte on its own
    free(s.buf);
    CHECK(decode(zs, zn, 1 << 20, sizeof(raw), &z, &s));
    CHECK(ota_inflate_feed(&z, zs, 0, sink_put, &s)); // an empty read after the end is no garbage
    CHECK(!ota_inflate_feed(&z, (uint8_t const *)"x", 1, sink_put, &s) && z.failed);
    CHECK(!ota_inflate_feed(&z, zs, 0, sink_put, &s)); // failed stays failed
    free(s.buf);
    for (size_t cut = 0; cut < zn; cut++) {
        uint8_t *exact = mut_exact(zs, cut);
        if (!exact) {
            exit(2);
        }
        decode(exact, cut, 1 << 20, sizeof(raw), &z, &s);
        CHECK(!ota_inflate_complete(&z, sizeof(raw)));
        free(s.buf);
        free(exact);
    }
    free(tail);
    free(zs);
}

// Seeded damage (bit flips, inserted bytes, deletions, repeats): never past the cap, never an overread, and a stream
// that still completes decodes to the original (zlib's Adler-32 catches the rest; the piece's SHA-256 is checked after)
static void test_mutations(void) {
    uint8_t raw[20000];
    fill(raw, sizeof(raw), 11);
    uint8_t *zs;
    size_t   zn = deflate_into(raw, sizeof(raw), &zs);
    static uint8_t buf[32768];
    mut_rng_t      rng;
    mut_seed(&rng, 298);
    for (int i = 0; i < 3000; i++) {
        size_t n;
        if (i % 3 == 0) {
            n = mut_truncate(zs, zn, zn, buf, sizeof(buf));
            for (int f = 0; f < 1 + (int)mut_below(&rng, 4); f++) {
                mut_flip_bit(buf, n, mut_below(&rng, n * 8));
            }
        } else {
            n = mut_random(&rng, zs, zn, buf, sizeof(buf));
        }
        uint8_t      *exact = mut_exact(buf, n);
        if (!exact) {
            exit(2);
        }
        ota_inflate_t z;
        sink_t        s;
        decode(exact, n, 1 + mut_below(&rng, 3000), sizeof(raw), &z, &s);
        CHECK(z.produced <= sizeof(raw) && s.len == z.produced);
        if (ota_inflate_complete(&z, sizeof(raw))) {
            CHECK(memcmp(s.buf, raw, sizeof(raw)) == 0);
        }
        free(s.buf);
        free(exact);
    }
    free(zs);
}

// A sink that refuses (a flash write failed) ends the stream for good
static void test_sink_refuses(void) {
    uint8_t raw[5000];
    fill(raw, sizeof(raw), 3);
    uint8_t *zs;
    size_t   zn = deflate_into(raw, sizeof(raw), &zs);
    zdec_t   d;
    z_open(&d);
    ota_inflate_t z;
    sink_t        s = sink_new(sizeof(raw));
    s.refuse_after  = 3;
    ota_inflate_begin(&z, z_step, &d, sizeof(raw));
    CHECK(!ota_inflate_feed(&z, zs, zn, sink_put, &s) && z.failed && s.len == 3 * sizeof(d.out));
    CHECK(z.produced == s.len); // the refused span is not counted as delivered
    inflateEnd(&d.zs);
    free(s.buf);
    free(zs);
}

// --- scripted decoders that break the step contract ---

typedef struct {
    ota_inflate_status_t status;
    size_t               take; // bytes it claims to take (may lie)
    size_t               give; // bytes it claims to produce
    bool                 null_out;
    int                  steps;
    uint8_t              out[64];
} script_t;

static ota_inflate_status_t script_step(void *dec, uint8_t const *in, size_t *in_len, uint8_t const **out,
                                        size_t *out_len) {
    (void)in;
    script_t *sc = dec;
    sc->steps++;
    *in_len  = sc->take;
    *out     = sc->null_out ? NULL : sc->out;
    *out_len = sc->give;
    return sc->status;
}

static bool run_script(script_t *sc, size_t len) {
    static uint8_t const in[16];
    ota_inflate_t        z;
    sink_t               s = sink_new(sizeof(sc->out));
    ota_inflate_begin(&z, script_step, sc, sizeof(sc->out));
    bool ok = ota_inflate_feed(&z, in, len, sink_put, &s);
    free(s.buf);
    return ok;
}

static void test_broken_decoders(void) {
    script_t sc = {.status = OTA_INFLATE_HAS_OUTPUT}; // neither takes nor gives: would spin forever
    CHECK(!run_script(&sc, 16) && sc.steps == 1);
    sc = (script_t){.status = OTA_INFLATE_NEEDS_INPUT, .take = 0}; // says it needs input but left some
    CHECK(!run_script(&sc, 16) && sc.steps == 1);
    sc = (script_t){.status = OTA_INFLATE_NEEDS_INPUT, .take = 17}; // takes more than it was given
    CHECK(!run_script(&sc, 16));
    sc = (script_t){.status = OTA_INFLATE_HAS_OUTPUT, .take = 16, .give = 65}; // more than the cap in one span
    CHECK(!run_script(&sc, 16));
    sc = (script_t){.status = OTA_INFLATE_NEEDS_INPUT, .take = 16, .give = 4, .null_out = true};
    CHECK(!run_script(&sc, 16));
    sc = (script_t){.status = OTA_INFLATE_ERROR, .take = 16, .give = 4}; // output of a broken stream is not delivered
    CHECK(!run_script(&sc, 16));
    sc = (script_t){.status = OTA_INFLATE_NEEDS_INPUT, .take = 16, .give = 64}; // exactly the cap, then needs more
    CHECK(run_script(&sc, 16));
}

int main(void) {
    test_round_trip();
    test_bomb();
    test_trailing_and_truncated();
    test_mutations();
    test_sink_refuses();
    test_broken_decoders();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ota_inflate_test: all passed\n");
    return 0;
}
