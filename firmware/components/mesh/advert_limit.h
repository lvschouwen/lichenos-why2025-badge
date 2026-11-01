#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Adverts from nodes not yet in the contact table, rate-limited (#6 item 8). Anyone can make keypairs and flood signed
// adverts; each one would take a contact slot (MeshCore overwrites the oldest non-favourite when the table is full).
// A token bucket: ADVERT_LIMIT_BURST new nodes at once, then one per ADVERT_LIMIT_EVERY_MS. Adverts from known contacts
// never pass through it. Pure logic, host-tested (host_tests/advert_limit_test.c).

#define ADVERT_LIMIT_BURST    32
#define ADVERT_LIMIT_EVERY_MS 3000

typedef struct {
    uint32_t tokens;
    uint32_t last_ms; // when tokens was last topped up
    uint32_t refused; // since boot
    uint8_t  primed;  // last_ms is set (the first take sets it)
    uint8_t  limiting; // the last take was refused
} advert_limit_t;

typedef enum {
    ADVERT_LIMIT_OK,            // take this advert
    ADVERT_LIMIT_REFUSED_FIRST, // dropped, the first since the last one taken: worth one log line
    ADVERT_LIMIT_REFUSED,       // dropped, as the one before
} advert_limit_result_t;

void advert_limit_init(advert_limit_t *l);
// One advert from an unknown node at now_ms (a wrapping millisecond clock)
advert_limit_result_t advert_limit_take(advert_limit_t *l, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
