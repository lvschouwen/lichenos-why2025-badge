#include "mutate.h"
#include "ota_pieces.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void hash(uint8_t const *d, size_t n, uint8_t out[32]) {
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, d, n);
    sha256_final(&c, out);
}

typedef struct {
    uint8_t const *mem;
    size_t         len;
    size_t         max_chunk; // the biggest read seen
} fake_t;

static int fake_read(void *ctx, size_t off, uint8_t *buf, size_t n) {
    fake_t *f = ctx;
    if (off + n > f->len) {
        return -1;
    }
    if (n > f->max_chunk) {
        f->max_chunk = n;
    }
    memcpy(buf, f->mem + off, n);
    return 0;
}

#define SHA "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"

// A piece request as tools/badge.sh sends it: offset, length, file size
static ota_request_t piece(ota_route_t route, long long offset, long long len, long long total) {
    ota_request_t r = {.route = route, .has_offset = true, .offset = offset, .content_length = len,
                       .total_size = total, .piece_size = -1, .has_sha256 = true, .has_file_sha256 = true};
    memcpy(r.sha256, SHA, sizeof(SHA));
    memcpy(r.file_sha256, SHA, sizeof(SHA));
    return r;
}

#define MAX (4LL * 1024 * 1024)

// #211: the checks handle_piece did inline, now pure; valid pieces pass, every refusal keeps its old answer
static void test_verdicts(void) {
    // The pieces of a real firmware push (128 KB pieces, ragged last), and of a file and an assets push
    for (long long off = 0; off < 1784656; off += 131072) {
        long long     len = 1784656 - off < 131072 ? 1784656 - off : 131072;
        ota_request_t r   = piece(OTA_ROUTE_FIRMWARE, off, len, 1784656);
        CHECK(ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_OK && ota_piece_bounds(&r) == OTA_PIECE_OK);
    }
    ota_request_t r = piece(OTA_ROUTE_FILE, 131072, 77, 131149);
    CHECK(ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_OK && ota_piece_bounds(&r) == OTA_PIECE_OK);
    r = piece(OTA_ROUTE_ASSETS, 0, 100, 3 * 1024 * 1024);
    CHECK(ota_piece_headers(&r, 3 * 1024 * 1024, 4096) == OTA_PIECE_OK && ota_piece_bounds(&r) == OTA_PIECE_OK);

    // Missing fields or compression
    ota_request_t m = piece(OTA_ROUTE_FILE, 0, 10, 10);
    m.has_sha256    = false;
    CHECK(ota_piece_headers(&m, MAX, 4096) == OTA_PIECE_MISSING);
    m = piece(OTA_ROUTE_FILE, 0, 10, 10), m.has_file_sha256 = false;
    CHECK(ota_piece_headers(&m, MAX, 4096) == OTA_PIECE_MISSING);
    m = piece(OTA_ROUTE_FILE, 0, -1, 10);
    CHECK(ota_piece_headers(&m, MAX, 4096) == OTA_PIECE_MISSING);
    m = piece(OTA_ROUTE_FILE, 0, 0, 10);
    CHECK(ota_piece_headers(&m, MAX, 4096) == OTA_PIECE_MISSING);
    m = piece(OTA_ROUTE_FILE, 0, 10, -1);
    CHECK(ota_piece_headers(&m, MAX, 4096) == OTA_PIECE_MISSING);
    m = piece(OTA_ROUTE_FILE, 0, 10, 10), m.deflate = true; // compressed without X-Piece-Size (#298)
    CHECK(ota_piece_headers(&m, MAX, 4096) == OTA_PIECE_BAD_DEFLATE);
    // Missing wins over too large, too large over unaligned, as the server answered before
    m = piece(OTA_ROUTE_FIRMWARE, 100, 10, MAX + 1), m.has_sha256 = false;
    CHECK(ota_piece_headers(&m, MAX, 4096) == OTA_PIECE_MISSING);
    m = piece(OTA_ROUTE_FIRMWARE, 100, 10, MAX + 1);
    CHECK(ota_piece_headers(&m, MAX, 4096) == OTA_PIECE_TOO_LARGE);
    m = piece(OTA_ROUTE_FIRMWARE, 100, 4096, MAX);
    CHECK(ota_piece_headers(&m, MAX, 4096) == OTA_PIECE_UNALIGNED);
    m = piece(OTA_ROUTE_FIRMWARE, 0, 5000, MAX);
    CHECK(ota_piece_headers(&m, MAX, 4096) == OTA_PIECE_UNALIGNED);
    m = piece(OTA_ROUTE_FILE, 100, 5000, MAX); // the sector rule is the firmware slot's only
    CHECK(ota_piece_headers(&m, MAX, 4096) == OTA_PIECE_OK);
    // An aligned firmware piece past the end is answered as past the end, not as unaligned (as before)
    m = piece(OTA_ROUTE_FIRMWARE, 8192, 8192, 10000);
    CHECK(ota_piece_headers(&m, MAX, 4096) == OTA_PIECE_OK && ota_piece_bounds(&m) == OTA_PIECE_PAST_END);

    // A misaligned firmware piece past the end, or past 4 GB, is answered as past the end (#211: one rule, the range
    // first; before, the 32-bit cast decided which of the two 400 texts it got)
    m = piece(OTA_ROUTE_FIRMWARE, 100, 5000, 1000);
    CHECK(ota_piece_headers(&m, MAX, 4096) == OTA_PIECE_OK && ota_piece_bounds(&m) == OTA_PIECE_PAST_END);
    m = piece(OTA_ROUTE_FIRMWARE, 4294967296LL, 100, MAX);
    CHECK(ota_piece_headers(&m, MAX, 4096) == OTA_PIECE_OK && ota_piece_bounds(&m) == OTA_PIECE_PAST_END);

    // Bounds: the last byte may be the file's last, one more is refused; no sum overflows
    m = piece(OTA_ROUTE_FILE, 90, 10, 100);
    CHECK(ota_piece_bounds(&m) == OTA_PIECE_OK);
    m = piece(OTA_ROUTE_FILE, 91, 10, 100);
    CHECK(ota_piece_bounds(&m) == OTA_PIECE_PAST_END);
    m = piece(OTA_ROUTE_FILE, 0, 101, 100);
    CHECK(ota_piece_bounds(&m) == OTA_PIECE_PAST_END);
    m = piece(OTA_ROUTE_FILE, 999999999999LL, 999999999999LL, 100);
    CHECK(ota_piece_bounds(&m) == OTA_PIECE_PAST_END);
    m = piece(OTA_ROUTE_FILE, -1, 10, 100);
    CHECK(ota_piece_bounds(&m) == OTA_PIECE_PAST_END);
    m = piece(OTA_ROUTE_FIRMWARE, 4294967296LL, 4096, MAX); // wrapped to 0 in a 32-bit size_t before #211
    CHECK(ota_piece_bounds(&m) == OTA_PIECE_PAST_END);
}

static void test_is_next(void) {
    ota_piece_expect_t e = {.active = true, .route = OTA_ROUTE_FILE, .total = 300, .got = 100, .path = "SD0:a.txt",
                            .file_sha256 = SHA};
    ota_request_t      r = piece(OTA_ROUTE_FILE, 100, 100, 300);
    CHECK(ota_piece_is_next(&e, &r, "SD0:a.txt"));
    CHECK(!ota_piece_is_next(&e, &r, "SD0:b.txt"));
    r.offset = 0;
    CHECK(!ota_piece_is_next(&e, &r, "SD0:a.txt"));
    r = piece(OTA_ROUTE_FILE, 100 + 4294967296LL, 100, 300); // would equal got after a 32-bit cast
    CHECK(!ota_piece_is_next(&e, &r, "SD0:a.txt"));
    r = piece(OTA_ROUTE_FILE, 100, 100, 300 + 4294967296LL);
    CHECK(!ota_piece_is_next(&e, &r, "SD0:a.txt"));
    r = piece(OTA_ROUTE_ASSETS, 100, 100, 300);
    CHECK(!ota_piece_is_next(&e, &r, "SD0:a.txt"));
    r             = piece(OTA_ROUTE_FILE, 100, 100, 300);
    r.file_sha256[0] = '0';
    CHECK(!ota_piece_is_next(&e, &r, "SD0:a.txt"));
    e.active = false;
    r        = piece(OTA_ROUTE_FILE, 100, 100, 300);
    CHECK(!ota_piece_is_next(&e, &r, "SD0:a.txt"));
}

// A compressed piece (#298): Content-Length counts the stream, X-Piece-Size the bytes it adds to the file
static ota_request_t zpiece(ota_route_t route, long long offset, long long wire, long long len, long long total) {
    ota_request_t r = piece(route, offset, wire, total);
    r.deflate       = true;
    r.piece_size    = len;
    return r;
}

// #298: compressed pieces pass the same rules in decoded bytes; failed before (any compressed piece was refused)
static void test_deflate_verdicts(void) {
    // A firmware push: the first piece plain, the others compressed to about 60 %
    long long const total = 2630000;
    for (long long off = 0; off < total; off += 131072) {
        long long     len = total - off < 131072 ? total - off : 131072;
        ota_request_t r   = off == 0 ? piece(OTA_ROUTE_FIRMWARE, 0, len, total) : zpiece(OTA_ROUTE_FIRMWARE, off, len * 6 / 10, len, total);
        CHECK(ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_OK && ota_piece_bounds(&r) == OTA_PIECE_OK);
        CHECK(ota_piece_len(&r) == len);
    }
    ota_request_t r = zpiece(OTA_ROUTE_FILE, 0, 20, 300, 300); // a file may start compressed
    CHECK(ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_OK && ota_piece_bounds(&r) == OTA_PIECE_OK);
    r = zpiece(OTA_ROUTE_ASSETS, 0, 20, 300, 3 * 1024 * 1024);
    CHECK(ota_piece_headers(&r, 3 * 1024 * 1024, 4096) == OTA_PIECE_OK && ota_piece_bounds(&r) == OTA_PIECE_OK);
    // Incompressible data comes out a little longer: allowed up to the bound, not past it
    r = zpiece(OTA_ROUTE_FILE, 0, ota_deflate_bound(1000), 1000, 1000);
    CHECK(ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_OK);
    CHECK(ota_deflate_bound(131072) >= 131072 + 131072 / 4096 + 131072 / 16384 + 13); // zlib's deflateBound
    r = zpiece(OTA_ROUTE_FILE, 0, ota_deflate_bound(1000) + 1, 1000, 1000);
    CHECK(ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_BLOATED);
    CHECK(ota_deflate_bound(-1) == 0 && ota_deflate_bound(999999999999LL) > 999999999999LL);

    // One compressed piece decodes to at most OTA_ZPIECE_MAX (badge.sh's piece cap): a tiny stream may not claim 4 MB
    r = zpiece(OTA_ROUTE_FIRMWARE, 262144, 1000, 256 * 1024, total);
    CHECK(OTA_ZPIECE_MAX == 256 * 1024);
    CHECK(ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_OK && ota_piece_bounds(&r) == OTA_PIECE_OK);
    r = zpiece(OTA_ROUTE_FILE, 0, 1000, 256 * 1024 + 1, MAX);
    CHECK(ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_ZTOO_LARGE);
    r = zpiece(OTA_ROUTE_FIRMWARE, 4096, 4096, 4 * 1024 * 1024 - 4096, MAX); // the whole slot in a few KiB
    CHECK(ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_ZTOO_LARGE);
    r = piece(OTA_ROUTE_FILE, 0, 1024 * 1024, MAX); // the cap is the compressed pieces' only: plain ones are as before
    CHECK(ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_OK);

    // X-Piece-Size exactly when compressed, and never 0
    r = piece(OTA_ROUTE_FILE, 0, 10, 10), r.piece_size = 10;
    CHECK(ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_BAD_DEFLATE);
    r = zpiece(OTA_ROUTE_FILE, 0, 10, 0, 10);
    CHECK(ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_BAD_DEFLATE);
    // The first firmware piece stays plain: its image header is checked before the slot is erased
    r = zpiece(OTA_ROUTE_FIRMWARE, 0, 100, 131072, total);
    CHECK(ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_FIRST_PLAIN);
    // Missing and too large still win; the sector rule and the bounds use the decoded length
    r = zpiece(OTA_ROUTE_FIRMWARE, 131072, 100, 131072, total), r.has_sha256 = false;
    CHECK(ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_MISSING);
    r = zpiece(OTA_ROUTE_FIRMWARE, 131072, 100, 131072, MAX + 1);
    CHECK(ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_TOO_LARGE);
    r = zpiece(OTA_ROUTE_FIRMWARE, 131072, 4096, 5000, total); // a 4096-byte stream of a ragged 5000-byte middle piece
    CHECK(ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_UNALIGNED);
    r = zpiece(OTA_ROUTE_FILE, 90, 5, 11, 100); // 5 wire bytes that would land one past the end
    CHECK(ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_OK && ota_piece_bounds(&r) == OTA_PIECE_PAST_END);
    r = zpiece(OTA_ROUTE_FILE, 90, 50, 10, 100); // more wire bytes than room left is fine: the decoded bytes fit
    CHECK(ota_piece_bounds(&r) == OTA_PIECE_OK);
    r = zpiece(OTA_ROUTE_FILE, 0, 10, 999999999999LL, 100);
    CHECK(ota_piece_bounds(&r) == OTA_PIECE_PAST_END);
}

// #211: random and boundary numbers (as a lying client would send) through the verdicts: whatever passes them lies in
// the file and under the limit, so the server's casts to size_t and its sums cannot wrap
static void test_verdict_lies(void) {
    static long long const edge[] = {-1, 0, 1, 4095, 4096, 4097, 131072, MAX - 1, MAX, MAX + 1, 4294967295LL,
                                     4294967296LL, 999999999999LL};
    size_t const           ne     = sizeof(edge) / sizeof(edge[0]);
    mut_rng_t              rng;
    mut_seed(&rng, 211);
    for (int i = 0; i < 20000; i++) {
        long long off   = i < 2197 ? edge[i % ne] : (long long)mut_next(&rng) % (MAX * 2);
        long long len   = i < 2197 ? edge[(i / ne) % ne] : (long long)mut_next(&rng) % (MAX * 2);
        long long total = i < 2197 ? edge[(i / ne / ne) % ne] : (long long)mut_next(&rng) % (MAX * 2);
        ota_route_t route = (i & 1) ? OTA_ROUTE_FIRMWARE : OTA_ROUTE_FILE;
        ota_request_t r   = piece(route, off, len, total);
        if (ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_OK && ota_piece_bounds(&r) == OTA_PIECE_OK) {
            CHECK(off >= 0 && len > 0 && total <= MAX && off + len <= total);
            CHECK(route != OTA_ROUTE_FIRMWARE ||
                  ota_piece_flash_ok((size_t)off, (size_t)len, (size_t)total, 4096));
        }
    }
}

// #298: the same lies through compressed pieces; whatever passes decodes to bytes inside the file and under the limit,
// and its stream is no longer than the bound, so the server's inflate cap and casts hold
static void test_deflate_lies(void) {
    static long long const edge[] = {-1, 0, 1, 4096, 131072, MAX - 1, MAX, MAX + 1, 4294967296LL, 999999999999LL};
    size_t const           ne     = sizeof(edge) / sizeof(edge[0]);
    mut_rng_t              rng;
    mut_seed(&rng, 298);
    for (int i = 0; i < 40000; i++) {
        bool      walk  = i < (int)(ne * ne * ne * ne);
        long long off   = walk ? edge[i % ne] : (long long)mut_next(&rng) % (MAX * 2);
        long long wire  = walk ? edge[(i / ne) % ne] : (long long)mut_next(&rng) % (MAX * 2);
        long long len   = walk ? edge[(i / ne / ne) % ne] : (long long)mut_next(&rng) % (MAX * 2);
        long long total = walk ? edge[(i / ne / ne / ne) % ne] : (long long)mut_next(&rng) % (MAX * 2);
        ota_route_t   route = (i & 1) ? OTA_ROUTE_FIRMWARE : OTA_ROUTE_FILE;
        ota_request_t r     = zpiece(route, off, wire, len, total);
        if (ota_piece_headers(&r, MAX, 4096) == OTA_PIECE_OK && ota_piece_bounds(&r) == OTA_PIECE_OK) {
            CHECK(off >= 0 && len > 0 && wire > 0 && total <= MAX && off + len <= total);
            CHECK(wire <= ota_deflate_bound(len) && ota_piece_len(&r) == len && len <= OTA_ZPIECE_MAX);
            CHECK(route != OTA_ROUTE_FIRMWARE ||
                  (off > 0 && ota_piece_flash_ok((size_t)off, (size_t)len, (size_t)total, 4096)));
        }
    }
}

// #298: a whole-body upload with a piece header is refused, not silently treated as plain
static void test_whole_body(void) {
    ota_request_t r = {.route = OTA_ROUTE_FIRMWARE, .content_length = 100, .total_size = -1, .piece_size = -1};
    CHECK(ota_whole_body_ok(&r));
    r.piece_size = 100;
    CHECK(!ota_whole_body_ok(&r));
    r.piece_size = 0;
    CHECK(!ota_whole_body_ok(&r));
}

// #298: the boot slot switches only for an image whose every byte came and whose slot hash is the file's hash
static void test_whole_file_ok(void) {
    char const *other = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ae";
    CHECK(ota_whole_file_ok(2630000, 2630000, SHA, SHA));
    CHECK(!ota_whole_file_ok(2630000, 2630000, SHA, other));        // a damaged piece, inflated or not
    CHECK(!ota_whole_file_ok(2630000 - 131072, 2630000, SHA, SHA)); // not every piece came
    CHECK(!ota_whole_file_ok(0, 0, SHA, SHA));
    CHECK(!ota_whole_file_ok(10, 10, "", ""));                      // no hash is no match
    CHECK(!ota_whole_file_ok(10, 10, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
                             "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
    CHECK(!ota_whole_file_ok(10, 10, SHA "0", SHA "0")); // too long
}

// #211: size arithmetic on recorded pieces refuses what would wrap instead of wrapping
static void test_overflow_guards(void) {
    CHECK(ota_round_up(SIZE_MAX, 128) == 0);
    CHECK(ota_round_up(SIZE_MAX - 126, 128) == 0);
    CHECK(ota_round_up(SIZE_MAX - 127, 128) == SIZE_MAX - 127); // already a multiple: the largest that fits
    CHECK(ota_round_up(5, 0) == 0);
    CHECK(ota_round_up(5, 3) == 0);
    CHECK(!ota_piece_flash_ok(0, 4096, 4096, 0));
    CHECK(!ota_piece_flash_ok(SIZE_MAX - 4095, 8192, 4096, 4096)); // offset + len wraps to below total
    CHECK(!ota_piece_flash_ok(8192, 4096, 4096, 4096));
    ota_pieces_t p = {0};
    uint8_t      d[32] = {0};
    CHECK(!ota_pieces_add(&p, (size_t)UINT32_MAX, 2, d));
    CHECK(ota_pieces_add(&p, (size_t)UINT32_MAX - 2, 2, d));
    // A recorded piece that reaches past the data is not read (ASan would see it) and counts as differing
    uint8_t *data = mut_exact((uint8_t const *)"0123456789", 10);
    size_t   bad[2];
    ota_pieces_clear(&p);
    hash(data, 10, d);
    CHECK(ota_pieces_add(&p, 0, 10, d));
    CHECK(ota_pieces_add(&p, 5, 10, d));
    CHECK(ota_pieces_check(&p, data, 10, bad, 2) == 1 && bad[0] == 1);
    free(data);
    ota_pieces_clear(&p);
}

int main(void) {
    test_verdicts();
    test_is_next();
    test_verdict_lies();
    test_deflate_verdicts();
    test_deflate_lies();
    test_whole_file_ok();
    test_whole_body();
    test_overflow_guards();

    CHECK(ota_round_up(0, 128) == 0);
    CHECK(ota_round_up(1, 128) == 128);
    CHECK(ota_round_up(128, 128) == 128);
    CHECK(ota_round_up(129, 128) == 256);
    CHECK(ota_round_up(1000001, 128) == 1000064);

    uint8_t data[1000];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 7 + 3);
    }
    ota_pieces_t p = {0};
    uint8_t      d[32];
    size_t       bad[4];
    for (size_t off = 0; off < sizeof(data); off += 300) {
        size_t n = sizeof(data) - off < 300 ? sizeof(data) - off : 300;
        hash(data + off, n, d);
        CHECK(ota_pieces_add(&p, off, n, d));
    }
    CHECK(p.n == 4);
    CHECK(ota_pieces_check(&p, data, sizeof(data), bad, 4) == 0);

    data[650] ^= 0x01; // piece 2 (600..899)
    data[999] ^= 0x80; // piece 3
    size_t cnt = ota_pieces_check(&p, data, sizeof(data), bad, 1);
    CHECK(cnt == 2);
    CHECK(bad[0] == 2);
    cnt = ota_pieces_check(&p, data, sizeof(data), bad, 4);
    CHECK(cnt == 2 && bad[0] == 2 && bad[1] == 3);

    // The same checks through a chunked reader (the OTA slot): small reads, same answers
    fake_t  f = {data, sizeof(data), 0};
    uint8_t scratch[64];
    cnt = ota_pieces_check_read(&p, fake_read, &f, scratch, sizeof(scratch), bad, 4);
    CHECK(cnt == 2 && bad[0] == 2 && bad[1] == 3);
    CHECK(f.max_chunk == sizeof(scratch));
    data[650] ^= 0x01;
    data[999] ^= 0x80;
    CHECK(ota_pieces_check_read(&p, fake_read, &f, scratch, sizeof(scratch), bad, 4) == 0);
    f.len = 700; // pieces 2 and 3 can't be read: they count as differing
    CHECK(ota_pieces_check_read(&p, fake_read, &f, scratch, sizeof(scratch), bad, 4) == 2);
    f.len = sizeof(data);
    uint8_t whole[32], expect[32];
    CHECK(ota_hash_range(fake_read, &f, 0, sizeof(data), scratch, sizeof(scratch), whole) == 0);
    hash(data, sizeof(data), expect);
    CHECK(memcmp(whole, expect, 32) == 0);
    CHECK(ota_hash_range(fake_read, &f, 10, sizeof(data), scratch, sizeof(scratch), whole) == -1);

    // Flash pieces: sector-aligned start; whole sectors long unless last
    CHECK(ota_piece_flash_ok(0, 131072, 1784656, 4096));
    CHECK(ota_piece_flash_ok(1703936, 80720, 1784656, 4096)); // the last, ragged piece
    CHECK(!ota_piece_flash_ok(1703936, 80000, 1784656, 4096));
    CHECK(!ota_piece_flash_ok(100, 4096, 1784656, 4096));
    CHECK(!ota_piece_flash_ok(0, 5000, 1784656, 4096));
    CHECK(!ota_piece_flash_ok(0, 0, 1784656, 4096));

    ota_pieces_clear(&p);
    CHECK(p.n == 0 && p.v == NULL);
    for (int i = 0; i < OTA_PIECES_MAX; i++) {
        CHECK(ota_pieces_add(&p, 0, 1, d));
    }
    CHECK(!ota_pieces_add(&p, 0, 1, d));
    ota_pieces_clear(&p);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ota_pieces_test: all checks passed\n");
    return 0;
}
