#include "log_store.h"

#include "log_clean.h"
#include "log_ring.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static log_ring_t     ring;
static bool           ready;
static volatile int   busy; // set while the ring changes; interrupts are off then (same scheme as log_tap.c)
static volatile uint32_t lost;
static vprintf_like_t previous;

// Never blocks: a writer that finds the ring in use drops its line (and counts it). The holder has interrupts off, so
// it can't be preempted and the other core is quick.
static bool take(UBaseType_t *state) {
    *state = portSET_INTERRUPT_MASK_FROM_ISR();
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) {
        portCLEAR_INTERRUPT_MASK_FROM_ISR(*state);
        return false;
    }
    return true;
}

static void give(UBaseType_t state) {
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    portCLEAR_INTERRUPT_MASK_FROM_ISR(state);
}

static void store(char const *text, size_t len) {
    uint32_t    ms = (uint32_t)(esp_timer_get_time() / 1000);
    UBaseType_t state;
    if (take(&state)) {
        log_ring_write(&ring, ms, text, len);
        give(state);
    } else {
        __atomic_fetch_add(&lost, 1, __ATOMIC_RELAXED);
    }
}

void log_store_add(char const *text, size_t len) {
    if (ready && len > 0) {
        store(text, len);
    }
}

static int hook(char const *fmt, va_list args) {
    if (ready) {
        char    text[LOG_RING_LINE_MAX + 2]; // on the caller's stack: no allocation, no shared buffer
        va_list copy;
        va_copy(copy, args);
        int n = vsnprintf(text, sizeof(text) - 1, fmt, copy);
        va_end(copy);
        if (n > 0) {
            size_t len      = (size_t)n < sizeof(text) - 1 ? (size_t)n : sizeof(text) - 2;
            bool   newline  = (size_t)n >= sizeof(text) - 1 || text[len - 1] == '\n'; // a cut-off line ends too
            // log_clean drops colour codes and line ends; esp_log sends one line per call, so a line end goes back
            len = log_clean(text, len, text);
            if (newline) {
                text[len++] = '\n';
            }
            store(text, len);
        }
    }
    return previous ? previous(fmt, args) : vprintf(fmt, args);
}

void log_store_start(void) {
    if (ready) {
        return;
    }
    uint8_t *mem = heap_caps_malloc(LOG_STORE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) {
        ESP_LOGW("log_store", "no PSRAM for the log store, GET /log stays empty");
        return;
    }
    log_ring_init(&ring, mem, LOG_STORE_BYTES);
    ready    = true;
    previous = esp_log_set_vprintf(hook);
}

size_t log_store_read(uint32_t since, char *out, size_t cap, uint32_t *next, uint32_t *missed) {
    *next   = since;
    *missed = 0;
    if (!ready) {
        return 0;
    }
    for (int attempt = 0; attempt < 1000; attempt++) {
        UBaseType_t state;
        if (take(&state)) {
            size_t n = log_ring_read(&ring, since, out, cap, next, missed);
            give(state);
            return n;
        }
    }
    return 0;
}

uint32_t log_store_lost(void) {
    return __atomic_load_n(&lost, __ATOMIC_RELAXED);
}
