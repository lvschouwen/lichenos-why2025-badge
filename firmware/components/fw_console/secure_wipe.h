#pragma once
// Wipes a buffer that held a password or a command with one (#123). The writes go through a volatile pointer, so the
// compiler cannot drop them as a dead store when the buffer is freed or goes out of scope right after (a plain memset
// can be). ESP-free; lives in fw_console because every component that handles typed text already requires it.
#include <stddef.h>
#include <stdint.h>

static inline void secure_wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) {
        *v++ = 0;
    }
}
