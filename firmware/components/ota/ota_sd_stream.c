#include "ota_sd_stream.h"

#include <string.h>

long long ota_sd_cap(bool free_known, uint64_t free_bytes, uint64_t written) {
    if (!free_known || free_bytes > (uint64_t)OTA_SD_TOTAL_MAX || written > (uint64_t)OTA_SD_TOTAL_MAX) {
        return OTA_SD_TOTAL_MAX;
    }
    uint64_t cap = free_bytes + written + OTA_SD_CAP_SLACK;
    return cap > (uint64_t)OTA_SD_TOTAL_MAX ? OTA_SD_TOTAL_MAX : (long long)cap;
}

bool ota_sd_stream_refuses(bool sd_active, ota_route_t route, bool has_offset, bool same_file) {
    if (!sd_active) {
        return false;
    }
    switch (route) {
        case OTA_ROUTE_FILE: return !(has_offset && same_file);
        case OTA_ROUTE_FIRMWARE:
        case OTA_ROUTE_ASSETS:
        case OTA_ROUTE_DELETE: return true;
        default: return false; // info, log, reboot, ...: nothing of the upload's
    }
}

bool ota_sd_orphan_is_live(char const *orphan, char const *live_temp, bool live) {
    return live && orphan[0] && strcmp(orphan, live_temp) == 0;
}

bool ota_whole_body_refused(ota_route_t route, long long content_length) {
    return route == OTA_ROUTE_FIRMWARE || (route == OTA_ROUTE_FILE && content_length > OTA_WHOLE_FILE_MAX);
}

bool ota_sd_piece_ok(long long len) {
    return len > 0 && len <= OTA_SD_PIECE_MAX;
}

bool ota_sd_stream_card_gone(bool mounted, uint32_t generation_now, uint32_t generation_at_begin) {
    return !mounted || generation_now != generation_at_begin;
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
