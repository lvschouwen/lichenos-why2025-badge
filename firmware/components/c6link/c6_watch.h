#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Noticing a C6 reset at runtime and pacing the re-attach (#14). Pure logic, no ESP headers: host-tested in
// host_tests/c6_watch_test.c. Every input returns a set of C6W_ACT_* bits; the supervisor (c6sup, #14 T-B) carries them
// out. The state is not locked here: the caller serialises the calls.
//
// UP -> (hard signal, confirmed soft signal, fw c6 reattach) -> ATTACHING(1) -> UP
//                                                                 \-> DOWN (wait 2 s, then 10 s) -> ATTACHING(n)
//                                                                 \-> after C6W_ATTEMPTS: GAVE_UP, one slow retry
//                                                                     every C6W_RETRY_SLOW_MS for as long as it takes

typedef enum { C6W_UP, C6W_DOWN, C6W_ATTACHING, C6W_GAVE_UP } c6w_state_t;

typedef enum {
    C6W_SRC_NONE,
    C6W_SRC_TRANSPORT, // ESP-Hosted posted TRANSPORT_FAILURE (hard)
    C6W_SRC_KEYBOARD,  // the TCA8418 lost its configuration: C6 EN went low (hard)
    C6W_SRC_LORA,      // LoRa sends kept failing and a probe got no answer (soft)
    C6W_SRC_REATTACH,  // fw c6 reattach
    C6W_SRC_HEARTBEAT, // C6W_HB_MISS_LIMIT heartbeats in a row did not come (#277)
    C6W_SRC_CP_INIT,   // the C6 announced a fresh boot while the link was up (#277)
} c6w_source_t;

enum {
    C6W_ACT_DOWN_HOOKS       = 1u << 0, // link_up false, stop the users of the link
    C6W_ACT_REATTACH         = 1u << 1, // try one ESP-Hosted re-attach, report it with c6_watch_attached()
    C6W_ACT_UP_HOOKS         = 1u << 2, // keyboard, backlight, LoRa again
    C6W_ACT_NOTE             = 1u << 3, // write c6_watch_t.reason to the restart history
    C6W_ACT_GIVE_UP          = 1u << 4, // show that the C6 is gone; the slow retry goes on
    C6W_ACT_FALLBACK_RESTART = 1u << 5, // offer one restart of the badge (Lucas: a warning dialog with a countdown)
    C6W_ACT_PROBE            = 1u << 6, // ask the C6 something cheap, report it with c6_watch_probe()
    C6W_ACT_HB_ARM           = 1u << 7, // ask the C6 for a heartbeat, report it with c6_watch_hb_armed()
    C6W_ACT_HB_MISSED        = 1u << 8, // log "heartbeat missed <hb_run>"
    C6W_ACT_CP_INIT          = 1u << 9, // log the unexpected C6 boot (cp_inits counts it)
};

#define C6W_ATTEMPTS         3
#define C6W_RETRY_SLOW_MS    (5u * 60u * 1000u)
#define C6W_SOFT_LIMIT       3      // LoRa send failures in a row before a probe
#define C6W_MANUAL_WINDOW_MS 30000u // an outage this soon after fw c6 reset is put down to it
// The C6 counts as rebooted when its uptime is at most the time spent down plus this: a reset is noticed late (the
// next SDIO transfer, which may be a LoRa poll 30 s away)
#define C6W_DETECT_SLACK_MS  35000u
#define C6W_UPTIME_UNKNOWN   UINT32_MAX
#define C6W_REASON_MAX       160
// Our own re-attach pulses C6 EN, which resets the TCA8418 too: a keyboard signal this soon after an outage ended is
// that pulse, not a new reset (#14 T-B)
#define C6W_KEYBOARD_GRACE_MS 3000u
// The NVS flag "the badge restarted for a lost C6" (c6_rt_restart) is cleared after this long with the C6 up
#define C6W_RT_FLAG_CLEAR_MS (10u * 60u * 1000u)

// Heartbeat (#277): the C6 sends one every C6W_HB_PERIOD_S once asked after a link-up. A beat counts as missed when
// the period plus C6W_HB_MARGIN_MS pass without one, then once per period; C6W_HB_MISS_LIMIT in a row (about 100 s) are
// an outage, but only with hb_act set: a new runtime guard counts and logs for one soak before it may act.
#define C6W_HB_PERIOD_S      30
#define C6W_HB_MARGIN_MS     10000u
#define C6W_HB_MISS_LIMIT    3
#define C6W_HB_RETRY_MS      60000u // a heartbeat request that failed is asked again this much later
// The C6's boot announcement of our own attach can arrive after the heartbeat request: only one later is unexpected
#define C6W_CP_INIT_GRACE_MS 10000u

typedef struct {
    bool     wanted;  // a link-up asked for the heartbeat (none before the first attach)
    bool     armed;   // the C6 accepted the request: beats are expected
    bool     act;     // C6W_HB_MISS_LIMIT misses start an outage; false = count and log only
    uint8_t  run;     // misses in a row
    uint32_t armed_ms;
    uint32_t last_ms; // the last beat, or the request, or the last miss counted
    uint32_t arm_ms;  // !armed: when to ask (again)
    uint32_t beats;   // since boot
    uint32_t misses;  // since boot
    uint32_t cp_inits; // unexpected C6 boot announcements since boot
} c6w_heartbeat_t;

typedef struct {
    c6w_state_t  state;
    c6w_source_t source;      // what started the current (or the last) outage
    uint8_t      attempt;     // re-attach attempts in this outage; above C6W_ATTEMPTS = slow retries
    uint8_t      soft_fails;  // LoRa send failures in a row
    bool         probing;     // a PROBE was asked for and not answered yet
    bool         trial_image; // a restart would roll the update back: never FALLBACK_RESTART
    bool         fallback_used;
    bool         manual_pending; // fw c6 reset pulsed EN at manual_ms
    bool         manual_cause;   // the current outage began manual_lead_ms after that pulse
    uint32_t     manual_ms;
    uint32_t     manual_lead_ms;
    uint32_t     down_ms;     // when the current outage began
    uint32_t     next_ms;     // DOWN / GAVE_UP: when the next attempt is due
    uint32_t     restarts;    // outages that ended with the C6 back
    uint32_t     last_recovery_ms;
    uint32_t     up_ms;       // when the last outage ended (valid once restarts > 0)
    char         reason[C6W_REASON_MAX]; // the last recovery, as the note and fw c6 show it ("" before the first)
    c6w_heartbeat_t hb;
} c6_watch_t;

// fallback_used: the badge already restarted for a lost C6 recently (an NVS flag the caller keeps)
void     c6_watch_init(c6_watch_t *w, bool trial_image, bool fallback_used);
// A hard signal starts an outage at once; repeats while not UP are ignored
unsigned c6_watch_hard(c6_watch_t *w, c6w_source_t src, uint32_t now_ms);
// A failed LoRa send: the C6W_SOFT_LIMIT-th in a row asks for a probe (once, until it is answered)
unsigned c6_watch_soft_fail(c6_watch_t *w, uint32_t now_ms);
void     c6_watch_soft_ok(c6_watch_t *w);
// The probe's result: no answer starts an outage (source LORA), an answer clears the count
unsigned c6_watch_probe(c6_watch_t *w, bool answered, uint32_t now_ms);
// fw c6 reset pulsed EN now: the outage it causes is labelled "after fw c6 reset" (within C6W_MANUAL_WINDOW_MS)
void     c6_watch_manual_reset(c6_watch_t *w, uint32_t now_ms);
// fw c6 reattach: starts an outage when UP, tries at once when DOWN or GAVE_UP, nothing while an attempt runs
unsigned c6_watch_reattach(c6_watch_t *w, uint32_t now_ms);
// The result of a REATTACH; c6_uptime_ms is the C6's own uptime (C6W_UPTIME_UNKNOWN when it could not be read)
unsigned c6_watch_attached(c6_watch_t *w, bool ok, uint32_t c6_uptime_ms, uint32_t now_ms);
// Starts the next attempt when it is due; counts a missed heartbeat (HB_MISSED) or asks for it again (HB_ARM)
unsigned c6_watch_tick(c6_watch_t *w, uint32_t now_ms);
// Heartbeat (#277). A link-up (the boot attach, every re-attach) asks for the heartbeat (HB_ARM) and forgets the old
// session's beats; the result of that request; a beat; an unexpected C6 boot announcement (CP_INIT logs it, and with
// hb.act set it starts an outage).
unsigned c6_watch_hb_link_up(c6_watch_t *w, uint32_t now_ms);
void     c6_watch_hb_armed(c6_watch_t *w, bool ok, uint32_t now_ms);
void     c6_watch_hb_beat(c6_watch_t *w, uint32_t now_ms);
unsigned c6_watch_cp_init(c6_watch_t *w, uint32_t now_ms);
// The `fw c6` heartbeat line: "c6: heartbeat every 30 s | beats 12 | misses 0 (log only)"
size_t   c6_watch_hb_line(c6_watch_t const *w, char *out, size_t cap);
// The `fw c6` line: "c6: up | restarts 1 | last: <reason>", "c6: recovering 2/3 | ...", "c6: down (no answer) | ...".
// Returns the length written (excluding the NUL).
size_t   c6_watch_line(c6_watch_t const *w, uint32_t now_ms, char *out, size_t cap);
// The state alone, as the status bar and About show it: "up", "recovering 2/3", "down (no answer)"
void     c6_watch_state_text(c6w_state_t state, uint8_t attempt, char *out, size_t cap);

// The supervisor queues every signal with the time it was raised and runs it later (#14 T-B). True when the signal
// belongs to an outage that is already over: raised before the C6 came back, or a keyboard reset that is only our
// own re-attach's EN pulse (within C6W_KEYBOARD_GRACE_MS of coming back). Such a signal is dropped.
bool     c6_watch_stale(c6_watch_t const *w, c6w_source_t src, uint32_t raised_ms);

// FALLBACK_RESTART, carried out (#14, Lucas 2026-10-31): normally a warning dialog with a countdown that ends in the
// restart; during a firmware update (nobody to ask, the update is lost with the C6 anyway) or without a screen to
// ask on, the restart at once
typedef enum { C6W_FALLBACK_ASK, C6W_FALLBACK_RESTART_NOW } c6w_fallback_t;
c6w_fallback_t c6_watch_fallback_plan(bool updating, bool can_ask);
// Whether the c6_rt_restart flag may be cleared now: it is set and the C6 has been up for C6W_RT_FLAG_CLEAR_MS
bool     c6_watch_rt_flag_clear_due(bool flag_set, c6w_state_t state, uint32_t uptime_ms);
