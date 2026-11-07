#pragma once
// One entry of the notification history (#62) as notify_os.h's lichen_notify_get copies it, and the field sizes the
// shell's centre and toast size their buffers by. A gate header (#245) so the shell needs no lichen_notify header;
// lichen_notify's notify_ring.h builds the history on it. Plain C types, no ESP headers.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NR_SLOTS       48
#define NR_SOURCE_CAP  14 // an app id is 1..13 of a-z 0-9 _ (app_ns.h), or "lichen"
#define NR_TARGET_CAP  32
#define NR_TITLE_CAP   40
#define NR_BODY_CAP    120

// Levels as lichen_notify.h's ln_level_t
#define NR_INFO  0
#define NR_WARN  1
#define NR_ERROR 2

typedef struct {
    uint32_t id;     // 0: a free slot
    uint32_t seq;    // order: higher is newer (a coalesced entry gets a new one, so it moves to the top)
    int64_t  epoch;  // wall clock (s) of the last change, 0 while the clock was not set
    uint32_t mono_s; // uptime (s) of the last change, in boot `boot`
    uint16_t boot;   // the store's boot counter when it last changed
    uint16_t count;  // notices merged into it (1 = just this one)
    uint8_t  level;
    uint8_t  flags;  // the lichen_notify flags it was raised with
    uint8_t  read;
    char     source[NR_SOURCE_CAP];
    char     target[NR_TARGET_CAP]; // "" for none
    char     title[NR_TITLE_CAP];
    char     body[NR_BODY_CAP];
} nr_entry_t;

#ifdef __cplusplus
}
#endif
