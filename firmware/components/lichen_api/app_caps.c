// The run-time capability check (#229), see app_caps.h
#include "app_caps.h"

#include "lichen_caps.h"

#include <stdio.h>
#include <string.h>

// By bit number: CAP_<name> of lichen_caps.h (host_tests/caps_test.c pins the order)
static char const *const CAP_NAMES[CAP_COUNT] = {
    "LORA_TX", "LORA_RX", "LORA_BORROW", "SD",      "NET",        "SECRETS", "RAW_KEYS", "CONSOLE", "RESTART",
    "BACKGROUND", "HAPTICS", "SENSORS",  "NOTIFY", "RADIO_SCAN", "BT",      "AUDIO",    "BATTERY", "SYSTEM",
};

bool app_caps_allows(uint32_t have, uint32_t need) {
    return (have & CAP_SYSTEM) || (have & need) == need;
}

bool app_caps_check(app_caps_refusals_t *r, app_ctx_table_t const *t, uint32_t ctx, uint32_t need,
                    app_caps_of_fn caps_of) {
    uint32_t slot = ctx & ((1u << APP_CTX_SLOT_BITS) - 1u);
    // The OS's context (slot 0) is never refused, and neither is a ctx the call's own check refuses (no double count)
    if (need == 0 || slot == 0 || !app_ctx_valid(t, ctx)) {
        return true;
    }
    void const *owner = t->owner[slot];
    if (!owner || app_caps_allows(caps_of(owner), need)) {
        return true;
    }
    __atomic_fetch_add(&r->refused, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&r->per_slot[slot], 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&r->last_need, need, __ATOMIC_RELAXED);
    return false;
}

bool app_caps_log_due(app_caps_refusals_t *r, uint32_t now_ms, uint32_t *skipped) {
    uint32_t last = __atomic_load_n(&r->logged_ms, __ATOMIC_RELAXED);
    bool     any  = __atomic_load_n(&r->logged_any, __ATOMIC_ACQUIRE) != 0;
    // One winner per gap: the compare-exchange settles two tasks refused at the same moment
    if ((any && now_ms - last < APP_CTX_LOG_GAP_MS) ||
        !__atomic_compare_exchange_n(&r->logged_ms, &last, now_ms, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        __atomic_fetch_add(&r->unlogged, 1u, __ATOMIC_RELAXED);
        return false;
    }
    __atomic_store_n(&r->logged_any, 1u, __ATOMIC_RELEASE);
    *skipped = __atomic_exchange_n(&r->unlogged, 0u, __ATOMIC_RELAXED);
    return true;
}

char const *app_caps_name(unsigned bit) {
    return bit < CAP_COUNT ? CAP_NAMES[bit] : NULL;
}

size_t app_caps_format(uint32_t caps, char *out, size_t cap) {
    if (!out || cap == 0) {
        return 0;
    }
    out[0]      = '\0';
    size_t used = 0;
    for (unsigned bit = 0; bit < CAP_COUNT && used + 1 < cap; bit++) {
        if (caps & (1u << bit)) {
            int n = snprintf(out + used, cap - used, "%s%s", used ? " " : "", CAP_NAMES[bit]);
            used += n > 0 ? (size_t)n : 0;
        }
    }
    if (used == 0) {
        snprintf(out, cap, "-");
        used = strlen(out);
    }
    return used < cap ? used : cap - 1;
}
