#include "settings_fail.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif

typedef struct {
    char     key[SETTINGS_FAIL_KEY_MAX + 1];
    int      err;
    uint32_t uptime_s;
} last_t;

static _Atomic uint32_t count;
static _Atomic uint32_t seq; // odd while a writer is in the slot
static last_t           last;

void settings_fail_record(char const *key, int err, uint32_t uptime_s) {
    uint32_t c = atomic_load(&count);
    while (c != UINT32_MAX && !atomic_compare_exchange_weak(&count, &c, c + 1)) {
    }
    uint32_t s = atomic_load(&seq);
    if ((s & 1) || !atomic_compare_exchange_strong(&seq, &s, s + 1)) {
        return; // another task is writing the slot: it is the latest enough
    }
    last_t n = {.err = err, .uptime_s = uptime_s};
    strncpy(n.key, key ? key : "", SETTINGS_FAIL_KEY_MAX);
    atomic_thread_fence(memory_order_release);
    last = n;
    atomic_store_explicit(&seq, s + 2, memory_order_release);
}

void settings_fail_note(char const *key, int err) {
#ifdef ESP_PLATFORM
    settings_fail_record(key, err, (uint32_t)(esp_timer_get_time() / 1000000));
#else
    settings_fail_record(key, err, 0);
#endif
}

uint32_t settings_fail_count(void) {
    return atomic_load(&count);
}

// A few tries is enough: a write of the slot is a 24-byte copy
static last_t snapshot(void) {
    last_t out = {0};
    for (int i = 0; i < 8; i++) {
        uint32_t s1 = atomic_load_explicit(&seq, memory_order_acquire);
        if (s1 & 1) {
            continue;
        }
        out = last;
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&seq, memory_order_relaxed) == s1) {
            break;
        }
    }
    return out;
}

void settings_fail_last_text(char *out, size_t cap) {
    if (cap == 0) {
        return;
    }
    last_t l = snapshot();
    if (l.key[0] == '\0' && l.err == 0) {
        out[0] = '\0';
        return;
    }
    char code[16];
    int  n = snprintf(code, sizeof(code), "0x%x", (unsigned)l.err);
    // The code stays whole; the key gives way
    size_t room = cap - 1 > (size_t)n + 1 ? cap - 1 - ((size_t)n + 1) : 0;
    size_t kl   = strlen(l.key);
    if (kl > room) {
        kl = room;
    }
    snprintf(out, cap, "%.*s %s", (int)kl, l.key, code);
}

uint32_t settings_fail_last_uptime_s(void) {
    return snapshot().uptime_s;
}

int settings_fail_last_err(void) {
    return snapshot().err;
}

void settings_fail_reset(void) {
    atomic_store(&count, 0);
    atomic_store(&seq, 0);
    memset(&last, 0, sizeof(last));
}

void settings_fail_preset_count(uint32_t n) {
    atomic_store(&count, n);
}
