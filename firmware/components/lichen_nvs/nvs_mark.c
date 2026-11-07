#include "nvs_mark.h"

#include <string.h>

nvs_mark_kind_t nvs_mark_parse(bool has_app, uint8_t const *bytes, size_t len) {
    if (!has_app) {
        return NVS_MARK_NO_APP;
    }
    static uint8_t const tag[8] = NVS_MARK_TAG;
    nvs_mark_t           m;
    if (!bytes || len < sizeof(m)) {
        return NVS_MARK_NONE;
    }
    memcpy(&m, bytes, sizeof(m));
    // A later version may add flags but keeps these two meaning the same
    if (memcmp(m.tag, tag, sizeof(tag)) != 0 || m.version < NVS_MARK_VERSION) {
        return NVS_MARK_NONE;
    }
    if (!(m.flags & NVS_MARK_F_ENCRYPT)) {
        return NVS_MARK_PLAIN;
    }
    return m.flags & NVS_MARK_F_BURN ? NVS_MARK_ENCRYPT_BURN : NVS_MARK_ENCRYPT;
}

bool nvs_mark_encrypts(nvs_mark_kind_t k) {
    return k == NVS_MARK_ENCRYPT || k == NVS_MARK_ENCRYPT_BURN;
}

bool nvs_marks_rollback_safe(nvs_mark_kind_t running, nvs_mark_kind_t other) {
    return nvs_mark_encrypts(running) && nvs_mark_encrypts(other);
}

char const *nvs_mark_name(nvs_mark_kind_t k) {
    switch (k) {
        case NVS_MARK_NO_APP: return "no app";
        case NVS_MARK_NONE: return "plain (built before the mark)";
        case NVS_MARK_PLAIN: return "plain";
        case NVS_MARK_ENCRYPT: return "encrypt";
        case NVS_MARK_ENCRYPT_BURN: return "encrypt + burn command";
    }
    return "?";
}
