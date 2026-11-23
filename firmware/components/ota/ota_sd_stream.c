#include "ota_sd_stream.h"

long long ota_sd_cap(bool free_known, uint64_t free_bytes, uint64_t written) {
    if (!free_known || free_bytes > (uint64_t)OTA_SD_TOTAL_MAX || written > (uint64_t)OTA_SD_TOTAL_MAX - free_bytes) {
        return OTA_SD_TOTAL_MAX;
    }
    return (long long)(free_bytes + written);
}

bool ota_sd_piece_ok(long long len) {
    return len > 0 && len <= OTA_SD_PIECE_MAX;
}

void ota_sd_stream_begin(ota_sd_stream_t *s, size_t total) {
    sha256_init(&s->run);
    s->cand  = s->run;
    s->got   = 0;
    s->total = total;
}

sha256_ctx_t *ota_sd_stream_piece(ota_sd_stream_t *s, size_t offset) {
    if (offset != s->got) {
        return NULL;
    }
    s->cand = s->run; // a piece that is then refused or resent leaves run as it was
    return &s->cand;
}

bool ota_sd_stream_accept(ota_sd_stream_t *s, size_t len) {
    if (len == 0 || len > s->total - s->got) {
        return false;
    }
    s->run  = s->cand;
    s->got += len;
    return true;
}

bool ota_sd_stream_final(ota_sd_stream_t const *s, uint8_t digest[SHA256_DIGEST_LEN]) {
    if (s->total == 0 || s->got != s->total) {
        return false;
    }
    sha256_ctx_t copy = s->run; // final() destroys its context
    sha256_final(&copy, digest);
    return true;
}
