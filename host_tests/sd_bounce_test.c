// Host test: the SD bounce path's rules (#121): which transfers are bounced, and how a transfer is cut into chunks
#include "sd_bounce.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_needed(void) {
    CHECK(sd_bounce_needed(true, true)); // PSRAM: never SDMMC DMA, aligned or not
    CHECK(sd_bounce_needed(true, false));
    CHECK(sd_bounce_needed(false, false)); // internal but unaligned: the DMA cannot use it as it is
    CHECK(!sd_bounce_needed(false, true)); // internal and aligned: the direct path stays
}

static void test_refused(void) {
    sd_bounce_iter_t it;
    CHECK(!sd_bounce_start(&it, 0, 8, 512, 511)); // not even one sector fits
    CHECK(!sd_bounce_start(&it, 0, 8, 0, 16384));
    CHECK(!sd_bounce_start(&it, 0, 8, 512, 0));
}

static void test_empty(void) {
    sd_bounce_iter_t  it;
    sd_bounce_chunk_t c;
    CHECK(sd_bounce_start(&it, 100, 0, 512, 16384));
    CHECK(!sd_bounce_next(&it, &c));
}

static void test_single_sector(void) {
    sd_bounce_iter_t  it;
    sd_bounce_chunk_t c;
    CHECK(sd_bounce_start(&it, 7, 1, 512, 16384));
    CHECK(sd_bounce_next(&it, &c));
    CHECK(c.sector == 7 && c.count == 1 && c.offset == 0 && c.bytes == 512);
    CHECK(!sd_bounce_next(&it, &c));
}

// The journal's 32 KB read: 64 sectors through 16 KB = two full chunks
static void test_exact_chunks(void) {
    sd_bounce_iter_t  it;
    sd_bounce_chunk_t c;
    CHECK(sd_bounce_start(&it, 1000, 64, 512, 16384));
    CHECK(sd_bounce_next(&it, &c));
    CHECK(c.sector == 1000 && c.count == 32 && c.offset == 0 && c.bytes == 16384);
    CHECK(sd_bounce_next(&it, &c));
    CHECK(c.sector == 1032 && c.count == 32 && c.offset == 16384 && c.bytes == 16384);
    CHECK(!sd_bounce_next(&it, &c));
}

static void test_partial_last(void) {
    sd_bounce_iter_t  it;
    sd_bounce_chunk_t c;
    CHECK(sd_bounce_start(&it, 0, 70, 512, 16384));
    CHECK(sd_bounce_next(&it, &c) && c.count == 32 && c.offset == 0);
    CHECK(sd_bounce_next(&it, &c) && c.count == 32 && c.offset == 16384 && c.sector == 32);
    CHECK(sd_bounce_next(&it, &c));
    CHECK(c.sector == 64 && c.count == 6 && c.offset == 32768 && c.bytes == 3072);
    CHECK(!sd_bounce_next(&it, &c));
}

// A bounce buffer that is not a whole number of sectors only ever carries whole sectors
static void test_odd_bounce(void) {
    sd_bounce_iter_t  it;
    sd_bounce_chunk_t c;
    CHECK(sd_bounce_start(&it, 0, 10, 512, 512 * 3 + 100));
    CHECK(it.per_chunk == 3);
    unsigned n = 0;
    while (sd_bounce_next(&it, &c)) {
        CHECK(c.bytes == (size_t)c.count * 512 && c.bytes <= 512 * 3 + 100);
        n++;
    }
    CHECK(n == 4); // 3 + 3 + 3 + 1
}

// Chunks are contiguous in the card and in the buffer and add up to the whole transfer, for any size
static void test_cover(void) {
    for (uint32_t count = 1; count <= 200; count++) {
        for (size_t bounce = 512; bounce <= 16384; bounce *= 2) {
            sd_bounce_iter_t  it;
            sd_bounce_chunk_t c;
            CHECK(sd_bounce_start(&it, 5000, count, 512, bounce));
            uint32_t sector = 5000, total = 0;
            size_t   offset = 0;
            while (sd_bounce_next(&it, &c)) {
                CHECK(c.sector == sector && c.offset == offset && c.count >= 1 && c.bytes <= bounce);
                sector += c.count;
                offset += c.bytes;
                total  += c.count;
            }
            CHECK(total == count && offset == (size_t)count * 512);
        }
    }
}

// The shipped size (#266): psram_dma's 32 KB SD piece (64 sectors) through the bounced leg is four 16-sector card
// commands
static void test_shipped_size(void) {
    sd_bounce_iter_t  it;
    sd_bounce_chunk_t c;
    CHECK(sd_bounce_start(&it, 2048, 64, 512, SD_BOUNCE_BYTES));
    int n = 0;
    while (sd_bounce_next(&it, &c)) {
        CHECK(c.count == 16 && c.bytes == 8192 && c.offset == (size_t)n * 8192 && c.sector == 2048u + 16u * (uint32_t)n);
        n++;
    }
    CHECK(n == 4);
}

// The mount's allocation sizes: SD_BOUNCE_BYTES, then halved, ending at SD_BOUNCE_MIN_BYTES; below that, stop
// (shipped: 8192, 4096, stop)
static void test_try_size(void) {
    CHECK(sd_bounce_try_size(0) == SD_BOUNCE_BYTES);
    unsigned attempt = 1;
    while (sd_bounce_try_size(attempt) != 0) {
        CHECK(sd_bounce_try_size(attempt) == sd_bounce_try_size(attempt - 1) / 2);
        attempt++;
    }
    CHECK(sd_bounce_try_size(attempt - 1) >= SD_BOUNCE_MIN_BYTES);
    CHECK(sd_bounce_try_size(attempt - 1) / 2 < SD_BOUNCE_MIN_BYTES);
    CHECK(sd_bounce_try_size(attempt + 1) == 0 && sd_bounce_try_size(1000) == 0);
}

int main(void) {
    test_needed();
    test_refused();
    test_empty();
    test_single_sector();
    test_exact_chunks();
    test_partial_last();
    test_odd_bounce();
    test_cover();
    test_shipped_size();
    test_try_size();
    if (failures) {
        printf("sd_bounce_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("sd_bounce_test: all passed\n");
    return 0;
}
