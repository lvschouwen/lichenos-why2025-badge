#include "nvs_mark.h"

#include <stdio.h>
#include <string.h>

nvs_mark_info_t nvs_mark_parse(bool has_app, uint8_t const *bytes, size_t len) {
    nvs_mark_info_t      info = {.app = has_app};
    static uint8_t const tag[8] = NVS_MARK_TAG;
    nvs_mark_t           m;
    if (!has_app || !bytes || len < sizeof(m)) {
        return info;
    }
    memcpy(&m, bytes, sizeof(m));
    // A later version may add flags but keeps these meaning the same
    if (memcmp(m.tag, tag, sizeof(tag)) != 0 || m.version < NVS_MARK_VERSION) {
        return info;
    }
    info.marked  = true;
    info.encrypt = m.flags & NVS_MARK_F_ENCRYPT;
    info.burn    = info.encrypt && (m.flags & NVS_MARK_F_BURN); // the burn command needs the option (Kconfig)
    info.guard   = m.flags & NVS_MARK_F_GUARD;
    return info;
}

static bool guarded(nvs_mark_info_t m) {
    return m.app && m.marked && m.guard;
}

nvs_slots_verdict_t nvs_slots_verdict(nvs_mark_info_t running, nvs_mark_info_t other) {
    if (!guarded(running) || !guarded(other)) {
        return NVS_SLOTS_UNSAFE;
    }
    return running.encrypt && other.encrypt ? NVS_SLOTS_SAFE : NVS_SLOTS_GUARDED;
}

void nvs_mark_describe(nvs_mark_info_t m, char *out, size_t cap) {
    if (!m.app) {
        snprintf(out, cap, "no app");
    } else if (!m.marked) {
        snprintf(out, cap, "no mark (built before it: unguarded)");
    } else {
        snprintf(out, cap, "%s%s, %s", m.encrypt ? "encrypt" : "plain", m.burn ? " + burn command" : "",
                 m.guard ? "guard" : "NO guard");
    }
}
