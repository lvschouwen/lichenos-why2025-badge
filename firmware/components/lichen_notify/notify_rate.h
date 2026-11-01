#pragma once
// Per-source rate limit and the show/hide decision of a new notice (#62). Pure, host-tested in
// host_tests/notify_rate_test.c; lichen_notify.c calls it under its mutex with the uptime in ms.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NRATE_SLOTS     8     // sources tracked at once; the least recently seen one makes room
#define NRATE_BURST     5     // notices a quiet source may raise in a row
#define NRATE_REFILL_MS 10000 // one more every 10 s
#define NRATE_SOURCE_CAP 14

typedef struct {
    struct {
        char     source[NRATE_SOURCE_CAP];
        uint8_t  tokens;
        uint32_t refill_ms; // uptime the last token was counted from
        uint32_t seen;      // for least-recently-seen eviction
    } slot[NRATE_SLOTS];
    uint32_t clock; // counts takes
} nrate_t;

// One notice from source at uptime now_ms (wraps after 49 days: only differences are used). True while the source is
// within its limit; a false one still lands in the history (restricted, coalescing) but gets no toast.
bool nrate_take(nrate_t *t, char const *source, uint32_t now_ms);

// Whether a new notice toasts and fires the feedback hooks (sound, vibration, LED)
typedef struct {
    bool toast;
    bool feedback;
} nrate_show_t;
// silent: LN_SILENT. Do-not-disturb hides everything but errors, which toast without feedback (Lucas, #62 Q2).
nrate_show_t nrate_show(uint8_t level, bool silent, bool dnd, bool within_rate);

#ifdef __cplusplus
}
#endif
