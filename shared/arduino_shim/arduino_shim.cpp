// Implementations behind the shim: rweather Crypto's RNG object (its own RNG.cpp needs the Arduino core; only rand()
// is used, for Ed25519/Curve25519 key generation) backed by the platform's random source, and ltoa.
#include "Arduino.h"
#include "RNG.h"

RNGClass       RNG;
HardwareSerial Serial;

RNGClass::RNGClass() : credits(0), firstSave(0), initialized(0), trngPending(0), timer(0), timeout(0), count(0), trngPosn(0) {}
RNGClass::~RNGClass() {}

void RNGClass::rand(uint8_t *data, size_t len) {
    shim_fill_random(data, len);
}

char *ltoa(long value, char *out, int base) {
    // MeshCore only formats in base 10
    (void)base;
    snprintf(out, 24, "%ld", value);
    return out;
}
