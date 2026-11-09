// Host test: advert blobs queued for a write after the mesh lock (#257)
#include "mesh_blobq.h"

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

static mesh_blobq_t q;

static void key_of(uint8_t *key, uint8_t n) {
    memset(key, 0, 32);
    key[0] = n;
    key[7] = (uint8_t)(n ^ 0x5a);
}

static void put_reads_back_and_replaces(void) {
    uint8_t key[32], out[MESH_BLOBQ_DATA], a[100], b[120];
    memset(a, 0xa1, sizeof(a));
    memset(b, 0xb2, sizeof(b));
    mesh_blobq_init(&q);
    key_of(key, 1);
    CHECK(mesh_blobq_get(&q, key, 32, out) == 0);
    CHECK(mesh_blobq_put(&q, key, 32, a, sizeof(a), 1000));
    CHECK(mesh_blobq_get(&q, key, 32, out) == sizeof(a) && memcmp(out, a, sizeof(a)) == 0);
    // The same node again: one entry, the newer packet, the age of the first
    CHECK(mesh_blobq_put(&q, key, 32, b, sizeof(b), 4000));
    CHECK(mesh_blobq_count(&q) == 1);
    CHECK(mesh_blobq_get(&q, key, 32, out) == sizeof(b) && memcmp(out, b, sizeof(b)) == 0);
    mesh_blobq_entry_t e;
    CHECK(!mesh_blobq_take(&q, 5999, false, &e));
    CHECK(mesh_blobq_take(&q, 6000, false, &e) && e.len == sizeof(b)); // 5 s after the first put, not the second
    // Only the first 8 bytes name it, as DataStore's file name does
    uint8_t other[32];
    memcpy(other, key, 32);
    other[20] = 0xff;
    CHECK(mesh_blobq_get(&q, other, 32, out) == sizeof(b));
}

static void refused_puts(void) {
    uint8_t key[32], data[MESH_BLOBQ_DATA + 1] = {0};
    mesh_blobq_init(&q);
    key_of(key, 2);
    CHECK(!mesh_blobq_put(&q, key, MESH_BLOBQ_KEY - 1, data, MESH_BLOBQ_MIN, 0));
    CHECK(!mesh_blobq_put(&q, key, 32, data, 0, 0));
    CHECK(!mesh_blobq_put(&q, key, 32, data, MESH_BLOBQ_MIN - 1, 0)); // not an advert
    CHECK(mesh_blobq_put(&q, key, 32, data, MESH_BLOBQ_MIN, 0));
    CHECK(!mesh_blobq_put(&q, key, 32, data, MESH_BLOBQ_DATA + 1, 0));
    CHECK(mesh_blobq_put(&q, key, 32, data, MESH_BLOBQ_DATA, 0));
    CHECK(mesh_blobq_get(&q, key, MESH_BLOBQ_KEY - 1, data) == 0);
    // Full: refused (the caller writes it), a queued key still replaces
    mesh_blobq_init(&q);
    for (uint8_t i = 0; i < MESH_BLOBQ_CAP; i++) {
        key_of(key, i);
        CHECK(mesh_blobq_put(&q, key, 32, data, MESH_BLOBQ_MIN, i));
    }
    key_of(key, MESH_BLOBQ_CAP);
    CHECK(!mesh_blobq_put(&q, key, 32, data, MESH_BLOBQ_MIN, 100));
    key_of(key, 3);
    CHECK(mesh_blobq_put(&q, key, 32, data, MESH_BLOBQ_MIN + 1, 100));
    CHECK(mesh_blobq_count(&q) == MESH_BLOBQ_CAP);
}

static void take_when_quiet_or_old(void) {
    uint8_t            k1[32], k2[32], d[MESH_BLOBQ_MIN] = {7};
    mesh_blobq_entry_t e;
    mesh_blobq_init(&q);
    CHECK(!mesh_blobq_take(&q, 0, true, &e));
    key_of(k1, 1);
    key_of(k2, 2);
    CHECK(mesh_blobq_put(&q, k2, 32, d, MESH_BLOBQ_MIN, 2000));
    CHECK(mesh_blobq_put(&q, k1, 32, d, MESH_BLOBQ_MIN, 1000));
    // A packet came in: not yet; quiet: at once, the one that waited longest first
    CHECK(!mesh_blobq_take(&q, 2500, false, &e));
    CHECK(mesh_blobq_take(&q, 2500, true, &e) && e.key[0] == 1);
    // Taken is not gone: still readable until it was written
    uint8_t out[MESH_BLOBQ_DATA];
    CHECK(mesh_blobq_get(&q, k1, 32, out) == MESH_BLOBQ_MIN);
    mesh_blobq_done(&q, &e);
    CHECK(mesh_blobq_get(&q, k1, 32, out) == 0 && mesh_blobq_count(&q) == 1);
    // A busy radio never starves it: written 5 s after its put
    CHECK(mesh_blobq_take(&q, 2000 + MESH_BLOBQ_MAX_AGE_MS, false, &e) && e.key[0] == 2);
    // Across the millisecond counter's wrap
    mesh_blobq_init(&q);
    CHECK(mesh_blobq_put(&q, k1, 32, d, MESH_BLOBQ_MIN, 0xFFFFFF00u));
    CHECK(!mesh_blobq_take(&q, 0x100, false, &e));
    CHECK(mesh_blobq_take(&q, MESH_BLOBQ_MAX_AGE_MS, false, &e));
}

static void newer_put_survives_the_write(void) {
    uint8_t            key[32], a[MESH_BLOBQ_MIN], b[MESH_BLOBQ_MIN + 2], out[MESH_BLOBQ_DATA];
    mesh_blobq_entry_t e;
    memset(a, 1, sizeof(a));
    memset(b, 2, sizeof(b));
    mesh_blobq_init(&q);
    key_of(key, 9);
    CHECK(mesh_blobq_put(&q, key, 32, a, sizeof(a), 0));
    CHECK(mesh_blobq_take(&q, 1, true, &e));
    CHECK(mesh_blobq_put(&q, key, 32, b, sizeof(b), 2)); // a newer advert while the older one was written
    mesh_blobq_done(&q, &e);
    CHECK(mesh_blobq_get(&q, key, 32, out) == sizeof(b)); // still waits for its own write
    CHECK(mesh_blobq_take(&q, 3, true, &e) && e.len == sizeof(b));
    mesh_blobq_done(&q, &e);
    CHECK(mesh_blobq_count(&q) == 0);
    // A factory reset: nothing queued is written afterwards
    CHECK(mesh_blobq_put(&q, key, 32, a, sizeof(a), 0));
    mesh_blobq_clear(&q);
    CHECK(mesh_blobq_count(&q) == 0 && !mesh_blobq_take(&q, 1, true, &e));
}

int main(void) {
    put_reads_back_and_replaces();
    refused_puts();
    take_when_quiet_or_old();
    newer_put_survives_the_write();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_blobq_test: all passed\n");
    return 0;
}
