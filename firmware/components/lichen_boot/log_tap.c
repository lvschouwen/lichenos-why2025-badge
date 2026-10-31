#include "log_tap.h"

#include "log_clean.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef char line_t[LOG_TAP_LINE_LEN];

static line_t          *ring; // allocated once, never freed
static unsigned         total; // lines ever stored
static volatile int     busy;  // set while the ring changes; interrupts are off then
static vprintf_like_t   previous;

// Never blocks: a writer that finds the ring in use (another core, or an interrupt that hit the holder) drops its
// line from the ring. The holder has interrupts off, so it can't be preempted and the other core is quick.
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

static int hook(char const *fmt, va_list args) {
    if (ring) {
        char    text[128]; // on the caller's stack: no allocation, no shared buffer
        va_list copy;
        va_copy(copy, args);
        int n = vsnprintf(text, sizeof(text), fmt, copy);
        va_end(copy);
        if (n > 0) {
            size_t len = log_clean(text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1, text);
            if (len > 0) {
                if (len > LOG_TAP_LINE_LEN - 1) {
                    len = LOG_TAP_LINE_LEN - 1;
                }
                UBaseType_t state;
                if (take(&state)) {
                    char *slot = ring[total % LOG_TAP_LINES];
                    memcpy(slot, text, len);
                    slot[len] = '\0';
                    total++;
                    give(state);
                }
            }
        }
    }
    return previous ? previous(fmt, args) : vprintf(fmt, args);
}

void log_tap_start(void) {
    if (ring) {
        return;
    }
    line_t *lines = heap_caps_calloc(LOG_TAP_LINES, sizeof(line_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lines) {
        ESP_LOGW("log_tap", "no PSRAM for the log ring, the boot screen shows no log");
        return;
    }
    ring     = lines;
    previous = esp_log_set_vprintf(hook);
}

size_t log_tap_last(char lines[][LOG_TAP_LINE_LEN], size_t max) {
    if (!ring || max == 0) {
        return 0;
    }
    for (int attempt = 0; attempt < 1000; attempt++) {
        UBaseType_t state;
        if (!take(&state)) {
            continue; // held by the other core for a few microseconds
        }
        size_t have = total < LOG_TAP_LINES ? total : LOG_TAP_LINES;
        size_t n    = have < max ? have : max;
        for (size_t i = 0; i < n; i++) {
            memcpy(lines[i], ring[(total - n + i) % LOG_TAP_LINES], LOG_TAP_LINE_LEN);
        }
        give(state);
        return n;
    }
    return 0;
}
