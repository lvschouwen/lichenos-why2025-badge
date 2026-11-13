// Host test: flash reads into PSRAM go through a small bounce buffer (#364): every read lands in the bounce buffer, none
// is larger than it, and the bytes arrive where they belong
#include "flash_chunk.h"

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

#define FLASH_LEN 10000

// A fake partition that records what the readers asked of it
typedef struct {
    uint8_t        bytes[FLASH_LEN];
    uint8_t const *only_into; // reads must land here (the bounce buffer)
    size_t         calls, largest, fail_at;
    int            wrong_target;
} fake_flash_t;

static int fake_read(void *ctx, size_t off, uint8_t *buf, size_t n) {
    fake_flash_t *f = ctx;
    f->calls++;
    f->largest      = n > f->largest ? n : f->largest;
    f->wrong_target += buf != f->only_into;
    if ((f->fail_at && f->calls == f->fail_at) || off + n > FLASH_LEN) {
        return -1;
    }
    memcpy(buf, f->bytes + off, n);
    return 0;
}

static void fake_init(fake_flash_t *f, uint8_t const *bounce) {
    memset(f, 0, sizeof(*f));
    for (size_t i = 0; i < FLASH_LEN; i++) {
        f->bytes[i] = (uint8_t)(i * 31 + 7);
    }
    f->only_into = bounce;
}

static void test_copy(void) {
    static fake_flash_t f;
    static uint8_t      dst[FLASH_LEN];
    uint8_t             bounce[FLASH_CHUNK_BOUNCE] = {0};
    fake_init(&f, bounce);
    // The assets case: one long read at an offset, a short last chunk
    CHECK(flash_chunk_copy(fake_read, &f, 100, dst, 9000, bounce, sizeof(bounce)) == 0);
    CHECK(memcmp(dst, f.bytes + 100, 9000) == 0);
    CHECK(f.calls == (9000 + FLASH_CHUNK_BOUNCE - 1) / FLASH_CHUNK_BOUNCE);
    CHECK(f.largest == FLASH_CHUNK_BOUNCE && f.wrong_target == 0);
    // Shorter than the bounce buffer: one read of exactly that size
    fake_init(&f, bounce);
    CHECK(flash_chunk_copy(fake_read, &f, 0, dst, 33, bounce, sizeof(bounce)) == 0);
    CHECK(f.calls == 1 && f.largest == 33 && memcmp(dst, f.bytes, 33) == 0);
    // Nothing to read: no call
    fake_init(&f, bounce);
    CHECK(flash_chunk_copy(fake_read, &f, 0, dst, 0, bounce, sizeof(bounce)) == 0 && f.calls == 0);
}

static void test_copy_fails(void) {
    static fake_flash_t f;
    static uint8_t      dst[FLASH_LEN];
    uint8_t             bounce[64] = {0};
    fake_init(&f, bounce);
    f.fail_at = 3; // the third chunk cannot be read: it stops there
    CHECK(flash_chunk_copy(fake_read, &f, 0, dst, 1000, bounce, sizeof(bounce)) == -1 && f.calls == 3);
    fake_init(&f, bounce);
    CHECK(flash_chunk_copy(fake_read, &f, 0, dst, 10, NULL, 64) == -1 && f.calls == 0);
    CHECK(flash_chunk_copy(fake_read, &f, 0, dst, 10, bounce, 0) == -1 && f.calls == 0);
}

static void test_same(void) {
    static fake_flash_t f;
    static uint8_t      expect[FLASH_LEN];
    uint8_t             bounce[FLASH_CHUNK_BOUNCE] = {0};
    fake_init(&f, bounce);
    memcpy(expect, f.bytes, FLASH_LEN);
    // The assets OTA read-back: equal, all through the bounce buffer
    CHECK(flash_chunk_same(fake_read, &f, 0, expect, FLASH_LEN, bounce, sizeof(bounce)) == 1);
    CHECK(f.largest == FLASH_CHUNK_BOUNCE && f.wrong_target == 0);
    CHECK(f.calls == (FLASH_LEN + FLASH_CHUNK_BOUNCE - 1) / FLASH_CHUNK_BOUNCE);
    // One byte off in the last, short chunk: differs
    expect[FLASH_LEN - 1] ^= 1;
    fake_init(&f, bounce);
    CHECK(flash_chunk_same(fake_read, &f, 0, expect, FLASH_LEN, bounce, sizeof(bounce)) == 0);
    // A difference in the first chunk stops there
    expect[FLASH_LEN - 1] ^= 1;
    expect[5] ^= 1;
    fake_init(&f, bounce);
    CHECK(flash_chunk_same(fake_read, &f, 0, expect, FLASH_LEN, bounce, sizeof(bounce)) == 0 && f.calls == 1);
    // An unreadable chunk is neither equal nor different
    fake_init(&f, bounce);
    f.fail_at = 2;
    CHECK(flash_chunk_same(fake_read, &f, 0, f.bytes, FLASH_LEN, bounce, sizeof(bounce)) == -1);
    CHECK(flash_chunk_same(fake_read, &f, 0, f.bytes, 10, NULL, 8) == -1);
}

int main(void) {
    test_copy();
    test_copy_fails();
    test_same();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("flash_chunk_test: all passed\n");
    return 0;
}
