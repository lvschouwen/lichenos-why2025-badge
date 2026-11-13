#include "ota_inflate.h"

void ota_inflate_begin(ota_inflate_t *z, ota_inflate_step_fn step, void *dec, size_t cap) {
    *z = (ota_inflate_t){.step = step, .dec = dec, .cap = cap};
}

static bool refuse(ota_inflate_t *z) {
    z->failed = true;
    return false;
}

bool ota_inflate_feed(ota_inflate_t *z, uint8_t const *in, size_t len, ota_inflate_sink_fn sink, void *ctx) {
    if (z->failed || (z->done && len > 0)) {
        return refuse(z); // nothing may follow the stream
    }
    while (!z->done) {
        size_t               took = len;
        uint8_t const       *out  = NULL;
        size_t               made = 0;
        ota_inflate_status_t st   = z->step(z->dec, in, &took, &out, &made);
        if (st == OTA_INFLATE_ERROR || took > len) {
            return refuse(z);
        }
        in  += took;
        len -= took;
        // The cap is checked before the sink sees a byte: a small stream must not expand past what the caller holds
        if (made > z->cap - z->produced || (made > 0 && !out)) {
            return refuse(z);
        }
        if (made > 0 && !sink(ctx, out, made)) {
            return refuse(z);
        }
        z->produced += made;
        if (st == OTA_INFLATE_DONE) {
            z->done = true;
            return len == 0 ? true : refuse(z);
        }
        if (st == OTA_INFLATE_NEEDS_INPUT && len == 0) {
            return true;
        }
        if (took == 0 && made == 0) {
            return refuse(z); // a decoder that neither takes nor gives would spin here forever
        }
    }
    return true; // done, and len was 0
}

bool ota_inflate_complete(ota_inflate_t const *z, size_t want) {
    return !z->failed && z->done && z->produced == want;
}
