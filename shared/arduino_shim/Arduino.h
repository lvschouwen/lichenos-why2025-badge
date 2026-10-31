// The bit of the Arduino core that MeshCore's protocol code and rweather's Crypto use, for the lean badge firmware
// (#61) and the host tests. Not a general Arduino port: add what a compile error asks for, nothing more.
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Stream.h"

#ifdef __cplusplus
extern "C" {
#endif

// Platform hooks: the firmware and the host tests each provide these
unsigned long millis(void);
void          shim_fill_random(void *buf, size_t len);
void          delay(unsigned long ms); // MeshCore's companion only (MyMesh's serial rescue CLI)

#ifdef __cplusplus
}
#endif

char *ltoa(long value, char *out, int base);

#ifdef __cplusplus
template <typename A, typename B> static inline auto min(A a, B b) -> decltype(a < b ? a : b) {
    return a < b ? a : b;
}
template <typename A, typename B> static inline auto max(A a, B b) -> decltype(a > b ? a : b) {
    return a > b ? a : b;
}
template <typename T, typename L, typename H> static inline T constrain(T v, L lo, H hi) {
    return v < (T)lo ? (T)lo : v > (T)hi ? (T)hi : v;
}

// Arduino's random(): from the platform's random source; MeshCore's StdRNG seeds it, which isn't needed here
static inline void randomSeed(unsigned long seed) {
    (void)seed;
}
static inline long random(long lo, long hi) {
    uint32_t r;
    shim_fill_random(&r, sizeof(r));
    return hi > lo ? lo + (long)(r % (uint32_t)(hi - lo)) : lo;
}
static inline long random(long hi) {
    return random(0, hi);
}

// Arduino's Serial: text goes to stdout (the firmware's log/console), nothing is ever read from it
class HardwareSerial : public Stream {
public:
    size_t write(uint8_t b) override { return putchar(b) == EOF ? 0 : 1; }
    int    read() override { return -1; }
    int    available() { return 0; }
};
extern HardwareSerial Serial;
#endif
