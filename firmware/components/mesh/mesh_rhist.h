#pragma once
// Repeater status history (#168): the badge asks a few repeaters the person chose for their status (the #51 request,
// mesh_remote.h's RR_STATUS) on a schedule and keeps the answers in a RAM ring for small graphs. Pure logic, no ESP
// headers (host_tests/mesh_rhist_test.c); not locked: mesh_rhist_run.c serialises every call with a spinlock.
//
// The poll transmits, so the schedule is careful by construction:
//  - nothing is asked for a repeater that was not switched on (rh_set); a fresh table asks nobody
//  - at most RH_SLOTS repeaters are on; each is asked at most once per interval (RH_INTERVAL_MIN or more), also when it
//    is switched off and on again (the next ask counts from the last one)
//  - at a start the switched-on repeaters wait RH_FIRST_S, RH_STAGGER_S apart, and any two asks are RH_GAP_S apart;
//    after a crash, a watchdog or a brownout each waits its whole interval, so a restart loop never asks more often
//  - the last ask of a repeater is remembered (RH_RECENT of them) when its slot goes to another one
//  - a repeater that does not answer is asked less often: the interval doubles per miss, up to RH_BACKOFF_MAX doublings
//  - never before the radio has its settings (#412): after a start MeshCore's settings go to the C6 within a second,
//    and a packet sent before them is cut short by them (the C6's RESULT code 3, a failed send). The radio counts as
//    ready once lora_settings_pending() has said no on ticks RH_SETTLE_S apart; a tick gap (MeshCore stopped) starts
//    that over. A poll refused because the radio was not ready is no ask and no miss: it waits RH_DEFER_S, doubling
//    per refusal in a row up to RH_DEFER_MAX times, and never comes before its regular time
//  - never while another request to a repeater runs (the one-at-a-time remote request), nor within RH_QUIET_S of a
//    request of the person, nor while a screen that asks a repeater (Log in, Status, Command, Settings) is open; a
//    request of the person replaces a poll that has not gone to MeshCore's pipe (mesh_remote.h, rr_request_background)
//
// Times are seconds since boot (mesh_uptime_s); the history is lost at a restart.
#include "mesh_remote.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Three repeaters: a person runs one or two of their own and maybe watches a neighbour's. Each ask is a request and an
// answer on a shared channel; three at the shortest interval is 36 short exchanges an hour, well under 0.1% airtime.
#define RH_SLOTS        3
#define RH_SAMPLES      144 // 24 h at 10 min, 6 days at 1 h
#define RH_PREFIX       6
#define RH_INTERVAL_MIN 10  // the shortest interval in minutes (the issue's floor)
#define RH_INTERVAL_DEF 30
#define RH_FIRST_S      180 // after a start, the first switched-on repeater waits this long
#define RH_STAGGER_S    120 // and each next one this much longer
#define RH_SOON_S       20  // a repeater switched on now is asked this soon (unless its last ask was within its interval)
#define RH_GAP_S        60  // between any two asks
#define RH_QUIET_S      120 // after a request of the person
#define RH_RETRY_S      60  // when the request could not start (LoRa off, MeshCore busy)
#define RH_SETTLE_S     2   // the radio seen ready this long (two ticks) first: its settings were sent before
#define RH_TICK_GAP_S   5   // ticks further apart: MeshCore was stopped, its next start sends the settings again
#define RH_DEFER_S      10  // a poll refused for a radio that was not ready: tried again after this
#define RH_DEFER_MAX    3   // doubling per refusal in a row, at most RH_DEFER_S x 8
#define RH_BACKOFF_MAX  3   // at most interval x 8 after misses
#define RH_RECENT       8   // last asks remembered past their slot
#define RH_NONE         INT32_MIN // a point the graph leaves out

// The intervals the screen and the console offer, in minutes
#define RH_INTERVALS 4
extern uint16_t const RH_INTERVAL_CHOICES[RH_INTERVALS];

typedef struct {
    uint32_t at_s;     // when the answer came
    uint32_t up_secs;  // the repeater's uptime
    uint32_t n_recv, n_sent;
    uint16_t batt_mv;  // 0: not reported
    int16_t  noise;    // dBm
} rh_sample_t;

typedef struct {
    bool        used, on;
    bool        console;    // switched on or changed by a console line (#290): its asks flood with the console scope
    uint8_t     prefix[RH_PREFIX];
    uint16_t    interval_min;
    uint8_t     misses;     // asks in a row without an answer
    uint8_t     defers;     // polls in a row refused for a radio that was not ready (#412): not misses
    uint32_t    next_s;     // when it is due
    uint32_t    ask_s;      // the last ask, 0 = never
    uint32_t    answer_s;   // the last answer, 0 = none
    uint32_t    asks, answers;
    char const *why;        // why the last ask got no answer or did not go out (static text), NULL when it was answered
    uint16_t    count, head; // samples held; head: where the next goes
    rh_sample_t s[RH_SAMPLES];
} rh_slot_t;

typedef struct {
    uint8_t  prefix[RH_PREFIX];
    uint32_t ask_s; // 0: unused
} rh_recent_t;

typedef struct {
    rh_slot_t   slot[RH_SLOTS];
    rh_recent_t recent[RH_RECENT]; // the last ask per repeater, also of those whose slot was taken
    uint32_t  last_ask_s; // any slot, for RH_GAP_S; 0 = none since start
    int8_t    asking;     // the slot whose request runs, -1 for none
    uint32_t  seq;        // its request's number
    uint32_t  seen_seq;   // the last status answer taken into a ring
    // The radio as the ticks saw it (#412, rh_radio): ready since ready_s, the last tick at tick_s
    bool      ready, ticked;
    uint32_t  ready_s, tick_s;
} rh_t;

typedef enum { RH_OK, RH_FULL, RH_BAD } rh_result_t;

// A fresh table: the radio not ready until rh_radio says it is
void rh_init(rh_t *t);
// The slot of this repeater, -1 when it has none
int rh_find(rh_t const *t, uint8_t const prefix[RH_PREFIX]);
size_t rh_on_count(rh_t const *t);
// Switches a repeater on (with its interval: RH_INTERVAL_CHOICES only) or off, or changes the interval of one that is
// on. On takes a free slot, or the off slot answered longest ago (its history goes); RH_FULL when RH_SLOTS are on.
// Off keeps the history until the slot is taken.
rh_result_t rh_set(rh_t *t, uint8_t const prefix[RH_PREFIX], bool on, uint16_t interval_min, uint32_t now_s);
// Who switched the repeater on or changed it last (#290): a console line's asks take the console scope. No-op without
// a slot.
void rh_set_console(rh_t *t, uint8_t const prefix[RH_PREFIX], bool console);
// The console scope changed or is gone (#290): every switched-on repeater a console line switched on goes off (its
// polls were meant for the old region; the history stays). Returns how many.
size_t rh_off_console(rh_t *t);
// The slot goes with its history (the contact was removed)
void rh_forget(rh_t *t, uint8_t const prefix[RH_PREFIX]);
// At a start: the switched-on repeaters from the settings, each asked first at RH_FIRST_S + i * RH_STAGGER_S, or after
// a crash restart (panic, watchdog, brownout) at its whole interval (at least RH_FIRST_S) + i * RH_STAGGER_S
void rh_load(rh_t *t, uint8_t const prefix[][RH_PREFIX], uint16_t const *interval_min, size_t n, uint32_t now_s,
             bool after_crash);
// The change asks the person first (careful dialog, YES on the console): switching on, or a shorter interval for one
// that is on (both raise how often it transmits). Switching off and a longer interval never ask.
bool rh_needs_confirm(rh_t const *t, uint8_t const prefix[RH_PREFIX], bool on, uint16_t interval_min);

// Each tick, before rh_due (#412): whether the radio has its settings now (no new settings waiting to go to the C6)
void rh_radio(rh_t *t, bool ready, uint32_t now_s);
// Ready for RH_SETTLE_S without a gap: rh_due asks nobody before
bool rh_radio_settled(rh_t const *t, uint32_t now_s);

// The wait after the last ask: the interval, doubled per miss up to RH_BACKOFF_MAX times
uint32_t rh_wait_s(rh_slot_t const *s);
// The slot to ask now, or -1 (also while the radio is not settled). busy: another remote request runs; other_age_s: how long ago the newest request that is
// not ours was asked (UINT32_MAX for none); held: a screen that asks a repeater is open.
int rh_due(rh_t const *t, uint32_t now_s, bool busy, uint32_t other_age_s, bool held);
// Our request for the slot started under seq / could not start (why: static text)
void rh_asked(rh_t *t, int idx, uint32_t seq, uint32_t now_s);
void rh_not_started(rh_t *t, int idx, char const *why, uint32_t now_s);
// Our request for the slot was refused because the radio was not ready (#412): not an ask, not a miss, the radio
// counts as not ready, and the slot waits RH_DEFER_S << (refusals in a row - 1, at most RH_DEFER_MAX), never less
// than it already waited for
void rh_not_ready(rh_t *t, int idx, uint32_t now_s);
// The remote request as it is now (mesh_remote_head's copy). Ends ours (an answer into the ring, a miss into the
// backoff), and takes a status answer to a request of the person into the ring of a switched-on repeater too (then it
// is not asked again for an interval). NULL: MeshCore is not there (ours is dropped without a miss). true when a ring
// or the schedule changed.
bool rh_observe(rh_t *t, rr_head_t const *h, uint32_t now_s);
// How long ago the person's newest request was asked, for rh_due: UINT32_MAX when the newest is ours or there is none
uint32_t rh_other_age_s(rh_t const *t, rr_head_t const *h);

// ---- the graphs ------------------------------------------------------------------------------------------------------
typedef enum { RH_BATTERY, RH_NOISE, RH_RECV_RATE, RH_SENT_RATE, RH_UPTIME, RH_FIELDS } rh_field_t;
// The samples oldest first; returns how many
size_t rh_samples(rh_slot_t const *s, rh_sample_t *out, size_t max);
// One value per sample, oldest first: battery in mV, noise in dBm, packets per hour since the sample before (RH_NONE
// for the first one and after the repeater restarted), uptime in hours. RH_NONE where there is nothing to draw.
void rh_series(rh_sample_t const *smp, size_t n, rh_field_t f, int32_t *out);
// The graph's bounds over the values that are not RH_NONE: battery widened to 100 mV, noise to 10 dB, counts and
// uptime from 0 (at least 10). false when there is no value at all.
bool rh_range(int32_t const *v, size_t n, rh_field_t f, int32_t *lo, int32_t *hi);

// ---- text --------------------------------------------------------------------------------------------------------------
// "10 min", "1 h", "3 h"
void rh_interval_text(uint16_t minutes, char *out, size_t cap);
// The interval after this one in RH_INTERVAL_CHOICES (round)
uint16_t rh_interval_next(uint16_t minutes);
bool     rh_interval_ok(uint16_t minutes);
// The Next ask row of a slot (asking: our request to it runs): "Off", "Asking...", "now", "in 7 min", "in 1 h 20 min"
void rh_next_text(rh_slot_t const *s, bool asking, uint32_t now_s, char *out, size_t cap);
// A value with its unit for a graph's note: "4.12 V", "-114 dBm", "120/h", "17 h"
void rh_value_text(rh_field_t f, int32_t v, char *out, size_t cap);

// ---- the setting: the switched-on repeaters as "a1b2c3d4e5f6:30,0a0b0c0d0e0f:10c" --------------------------------------
// A trailing 'c' marks one a console line switched on (#290), so its asks keep the console scope after a restart.
#define RH_CFG_MAX (RH_SLOTS * (2 * RH_PREFIX + 7) + 1)
void   rh_cfg_encode(rh_t const *t, char *out, size_t cap);
// Up to max entries; a bad entry (not 12 hex digits, an interval not offered) is skipped. Returns how many. console
// (may be NULL): each entry's 'c' mark.
size_t rh_cfg_decode(char const *text, uint8_t prefix[][RH_PREFIX], uint16_t *interval_min, bool *console, size_t max);

#ifdef __cplusplus
}
#endif
