#include "app_ns.h"

#include <stdio.h>
#include <string.h>

#define APP_ID_MAX 13 // "a." + id fits NVS's 15-character namespace

bool app_ns_name(char const *id, char *out, size_t cap) {
    if (!id || cap < APP_ID_MAX + 3) {
        return false;
    }
    size_t n = strlen(id);
    if (n == 0 || n > APP_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    snprintf(out, cap, "a.%s", id);
    return true;
}

app_adopt_t app_adopt_plan(bool have_app, bool have_legacy) {
    if (!have_legacy) {
        return APP_ADOPT_NONE;
    }
    return have_app ? APP_ADOPT_DROP_LEGACY : APP_ADOPT_COPY;
}

app_adopted_t app_adopt_run(app_adopt_io_t const *io, uint32_t *legacy) {
    uint32_t app_value = 0;
    uint32_t old_value = 0;
    app_key_read_t app_read    = io->get_app(io->ctx, &app_value);
    app_key_read_t legacy_read = io->get_legacy(io->ctx, &old_value);
    if (legacy) {
        *legacy = old_value;
    }
    // An unreadable key is not a missing one: taken for missing, a newer app value would be overwritten by the old key
    if (app_read == APP_KEY_ERROR || legacy_read == APP_KEY_ERROR) {
        return APP_ADOPTED_SKIPPED;
    }
    switch (app_adopt_plan(app_read == APP_KEY_FOUND, legacy_read == APP_KEY_FOUND)) {
        case APP_ADOPT_NONE: return APP_ADOPTED_NOTHING;
        case APP_ADOPT_DROP_LEGACY: return io->erase_legacy(io->ctx) ? APP_ADOPTED_DROPPED : APP_ADOPTED_DROP_FAILED;
        case APP_ADOPT_COPY: break;
    }
    uint32_t back = 0;
    if (io->set_app(io->ctx, old_value) && io->get_app(io->ctx, &back) == APP_KEY_FOUND && back == old_value) {
        return APP_ADOPTED_COPIED;
    }
    // A copy that is there but wrong would win over the old key on the next boot (DROP_LEGACY): take it out again
    io->erase_app(io->ctx);
    return APP_ADOPTED_COPY_FAILED;
}
