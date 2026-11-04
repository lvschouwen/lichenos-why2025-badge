#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Who holds the radio (#84 T7, #179), and how often a refused caller is logged. Pure logic, no ESP headers: lora.c
// keeps one of each under its owner_lock (the names are short literals, the strcmp is cheap enough for a spinlock).

typedef struct {
    char const *name; // NULL: nobody holds the radio
} lora_owner_t;

// Free, or already who's: who holds it now. Otherwise refused (false) and nothing changes. A NULL who is refused.
bool lora_owner_claim(lora_owner_t *o, char const *who);
// True when who held it (now free); anyone else changes nothing
bool lora_owner_release(lora_owner_t *o, char const *who);
// True only when who (not NULL) holds the radio: every owner-only call checks this first
bool lora_owner_is(lora_owner_t const *o, char const *who);

// A caller that keeps sending without the radio must not flood the log: one line per LORA_REFUSAL_LOG_MS at most,
// with the count of refusals not logged since the last line
#define LORA_REFUSAL_LOG_MS 10000

typedef struct {
    bool     logged; // a line went out already (the first refusal is always logged)
    uint32_t last_ms;
    uint32_t skipped;
} lora_refusal_log_t;

// One refusal at now_ms: true when it gets a log line, with *skipped the refusals swallowed since the last line
bool lora_refusal_due(lora_refusal_log_t *r, uint32_t now_ms, uint32_t *skipped);

#ifdef __cplusplus
}
#endif
