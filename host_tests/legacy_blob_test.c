// legacy_blob_run (#123): the identity hand-over reads first and erases only after the app confirmed the store.
#include "legacy_blob.h"

#include <assert.h>
#include <string.h>

typedef struct {
    lb_read_t   read_result;
    size_t      blob_len;
    int         erases;
    bool        erase_ok;
    int         stores;
    bool        store_ok;
    uint8_t     stored[8];
    size_t      stored_len;
    int         erases_at_store;
} fake_t;

static lb_read_t fake_read(void *ctx, uint8_t *buf, size_t cap, size_t *len) {
    fake_t *f = ctx;
    if (f->read_result == LB_READ_FOUND) {
        assert(f->blob_len <= cap);
        memset(buf, 0xAB, f->blob_len);
        *len = f->blob_len;
    }
    return f->read_result;
}
static bool fake_erase(void *ctx) {
    fake_t *f = ctx;
    f->erases++;
    return f->erase_ok;
}
static bool fake_store(void *ctx, uint8_t const *blob, size_t len) {
    fake_t *f = ctx;
    f->stores++;
    f->erases_at_store = f->erases;
    f->stored_len      = len;
    memcpy(f->stored, blob, len < sizeof(f->stored) ? len : sizeof(f->stored));
    return f->store_ok;
}

static lichen_blob_t run(fake_t *f, size_t min_len) {
    lb_io_t io = {.ctx = f, .read = fake_read, .erase = fake_erase};
    uint8_t buf[16];
    return legacy_blob_run(&io, buf, sizeof(buf), min_len, fake_store, f);
}

int main(void) {
    fake_t f = {.read_result = LB_READ_FOUND, .blob_len = 8, .erase_ok = true, .store_ok = true};
    assert(run(&f, 4) == LICHEN_BLOB_MOVED);
    assert(f.stores == 1 && f.erases == 1 && f.erases_at_store == 0 && f.stored_len == 8 && f.stored[0] == 0xAB);

    f = (fake_t){.read_result = LB_READ_FOUND, .blob_len = 8, .erase_ok = true, .store_ok = false};
    assert(run(&f, 4) == LICHEN_BLOB_KEPT); // the app could not store it: nothing erased
    assert(f.stores == 1 && f.erases == 0);

    f = (fake_t){.read_result = LB_READ_FOUND, .blob_len = 8, .erase_ok = false, .store_ok = true};
    assert(run(&f, 4) == LICHEN_BLOB_ERASE_FAILED);

    f = (fake_t){.read_result = LB_READ_ERROR, .erase_ok = true, .store_ok = true};
    assert(run(&f, 4) == LICHEN_BLOB_NONE); // a read error never erases and never reaches the app
    assert(f.stores == 0 && f.erases == 0);

    f = (fake_t){.read_result = LB_READ_MISSING, .erase_ok = true, .store_ok = true};
    assert(run(&f, 4) == LICHEN_BLOB_NONE && f.stores == 0 && f.erases == 0);

    f = (fake_t){.read_result = LB_READ_FOUND, .blob_len = 3, .erase_ok = true, .store_ok = true};
    assert(run(&f, 4) == LICHEN_BLOB_NONE && f.stores == 0 && f.erases == 0); // too short: left alone

    f = (fake_t){.read_result = LB_READ_FOUND, .blob_len = 4, .erase_ok = true, .store_ok = true};
    assert(run(&f, 4) == LICHEN_BLOB_MOVED); // exactly min_len is enough

    assert(legacy_blob_allowed("meshcore", "identity"));
    assert(!legacy_blob_allowed("meshcore", "tz"));
    assert(!legacy_blob_allowed("notes", "identity"));
    assert(!legacy_blob_allowed(NULL, "identity") && !legacy_blob_allowed("meshcore", NULL));
    return 0;
}
