#pragma once
// Wipes a buffer that held a password or a command with one (#123). The writes go through a volatile pointer, so the
// compiler cannot drop them as a dead store when the buffer goes out of scope right after (a plain memset can be).
// ESP-free (host tests build the code that uses it).
#include <stddef.h>
#include <stdint.h>

static inline void mesh_wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) {
        *v++ = 0;
    }
}
