#include "settings_fail.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

typedef struct {
    uint32_t count;
    char     key[SETTINGS_FAIL_KEY_MAX + 1];
    int      err;
    uint32_t uptime_s;
} state_t;

// One lock over the whole state so the count and the text are always one failure apart at most: ~28 bytes in internal
// .bss, copied inside the critical section and nothing else (no log, no clock, no PSRAM). The host test is
// single-threaded, so the shim is empty there.
#ifdef ESP_PLATFORM
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   portENTER_CRITICAL(&lock)
#define UNLOCK() portEXIT_CRITICAL(&lock)
#else
#define LOCK()
#define UNLOCK()
#endif

static state_t st;

void settings_fail_record(char const *key, int err, uint32_t uptime_s) {
    state_t n = {.err = err, .uptime_s = uptime_s};
    strncpy(n.key, key ? key : "", SETTINGS_FAIL_KEY_MAX);
    LOCK();
    n.count = st.count == UINT32_MAX ? UINT32_MAX : st.count + 1;
    st      = n;
    UNLOCK();
}

void settings_fail_note(char const *key, int err) {
#ifdef ESP_PLATFORM
    settings_fail_record(key, err, (uint32_t)(esp_timer_get_time() / 1000000));
#else
    settings_fail_record(key, err, 0);
#endif
}

static state_t snapshot(void) {
    LOCK();
    state_t out = st;
    UNLOCK();
    return out;
}

uint32_t settings_fail_count(void) {
    return snapshot().count;
}

void settings_fail_last_text(char *out, size_t cap) {
    if (cap == 0) {
        return;
    }
    state_t l = snapshot();
    if (l.count == 0) {
        out[0] = '\0';
        return;
    }
    char code[16];
    // ESP_FAIL is -1: shown as "-1", not 0xffffffff, which would eat the key's room
    int n = l.err < 0 ? snprintf(code, sizeof(code), "%d", l.err) : snprintf(code, sizeof(code), "0x%x", (unsigned)l.err);
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
    LOCK();
    memset(&st, 0, sizeof(st));
    UNLOCK();
}

void settings_fail_preset_count(uint32_t n) {
    LOCK();
    st.count = n;
    UNLOCK();
}
