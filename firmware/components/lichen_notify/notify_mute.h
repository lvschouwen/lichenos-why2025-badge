#pragma once
// Per-source on/off switch for notifications (#62, Lucas: full control over what reaches the centre; per-target rules
// are the app's own business, #50). A source that is off has its notices dropped: no history, no toast, no badge;
// they are counted as muted. Default on. The switch is kept in NVS (badge_fw) under NMUTE key "n.<source>" (NVS keys
// are at most 15 characters, an app id 13); this table caches what was read. Pure, host-tested in
// host_tests/notify_mute_test.c; lichen_notify.c adds the NVS reads and writes under its mutex.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NMUTE_SLOTS      24 // every registered app (16) and "lichen", "test" and a few strays
#define NMUTE_SOURCE_CAP 14
#define NMUTE_KEY_CAP    16

typedef enum { NMUTE_UNKNOWN, NMUTE_ON, NMUTE_OFF } nmute_state_t;

typedef struct {
    struct {
        char source[NMUTE_SOURCE_CAP];
        bool on;
    } slot[NMUTE_SLOTS];
    size_t   used;
    uint32_t muted; // notices dropped because their source is off
} nmute_t;

// "n.<source>" for NVS; false for a source that does not fit (then it can't be switched off and is always on)
bool          nmute_key(char const *source, char *out, size_t cap);
// What the cache knows; UNKNOWN means read the store, then nmute_put what it said (default on)
nmute_state_t nmute_get(nmute_t const *t, char const *source);
// Caches a switch; a full cache forgets its oldest entry (it is read from the store again when needed)
void          nmute_put(nmute_t *t, char const *source, bool on);
// A notice from source with the switch known: true when it must be dropped (counted in muted)
bool          nmute_drop(nmute_t *t, char const *source);

#ifdef __cplusplus
}
#endif
