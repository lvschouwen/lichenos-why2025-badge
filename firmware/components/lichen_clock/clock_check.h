#pragma once
// Which wall-clock times to believe (#8), and from which source (#83). Pure, host-tested.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCK_BELOW_BUILD_S (24 * 3600)        // a clock a day behind the build is still plausible
#define CLOCK_ABOVE_BUILD_S (2 * 365 * 24 * 3600LL) // the firmware is not trusted to run longer than this unsynced
#define CLOCK_MAX_STEP_S    (10 * 60)          // after a good sync, later SNTP answers may only nudge the clock

#define CLOCK_RECOVER_ANSWERS 3  // this many big steps in a row that agree...
#define CLOCK_RECOVER_AGREE_S 60 // ...within this many seconds of each other are believed after all
#define CLOCK_RECOVER_SPACE_S (30 * 60) // and each at least this long after the one counted before (no bursts)

// CLOCK_RECOVERED: accepted too, a big step that several answers in a row agreed on. CLOCK_OUTRANKED: a higher-ranked
// source set the clock this boot (#83); CLOCK_NOT_FORWARD: an app may only move a plausible clock forward (#82).
typedef enum {
    CLOCK_OK,
    CLOCK_TOO_OLD,
    CLOCK_TOO_NEW,
    CLOCK_BIG_STEP,
    CLOCK_RECOVERED,
    CLOCK_OUTRANKED,
    CLOCK_NOT_FORWARD,
    CLOCK_NO_SOURCE,
} clock_verdict_t;

// Who set the clock (#83), in rank order: a source only applies while no higher-ranked one has set the clock this
// boot. RTC (a plausible time a warm reset carried over) ranks lowest of the real sources: an app may only move a
// plausible clock forward anyway, so the carried-over clock is never rewound by MeshCore's bootstrap from contacts
// (#82), while a later contact or companion time may still correct an RTC that lost time. NONE and RTC are states,
// not proposers.
typedef enum { CLOCK_SRC_NONE, CLOCK_SRC_RTC, CLOCK_SRC_APP, CLOCK_SRC_MANUAL, CLOCK_SRC_SNTP } clock_src_t;

typedef struct {
    int     count;  // consecutive big-step answers that agreed so far
    int64_t offset; // the offset (answer minus clock) of the latest of them
    int64_t at_s;   // monotonic time (s) when that one was counted
} clock_guard_t;

// true when t lies in build_epoch - 1 day .. build_epoch + 2 years
bool clock_in_window(int64_t t, int64_t build_epoch);

// An SNTP answer cand against the current time now. synced: SNTP has already set the time since this boot, so a jump
// of more than 10 minutes is refused (an on-path attacker could otherwise move the clock later; real drift is ms).
clock_verdict_t clock_check(int64_t cand, int64_t now, int64_t build_epoch, bool synced);

// clock_check() plus the way out of a lock-in: the first answer after a sync may have been wrong (inside the window),
// and every right answer after it is a "big step". CLOCK_RECOVERED after CLOCK_RECOVER_ANSWERS agreeing big steps in a
// row, each 30 minutes after the last counted one (mono_s: monotonic seconds, so retries and bursts do not count); a small step, an implausible answer or an answer that disagrees restarts the count, so one rogue answer never
// moves the clock.
clock_verdict_t clock_check_guarded(clock_guard_t *g, int64_t cand, int64_t now, int64_t build_epoch, bool synced,
                                    int64_t mono_s);

// The arbitration behind clock_propose() (#83): may src set the clock to cand, given the source that set it so far
// (cur: the highest-ranked one this boot, or NONE/RTC at boot)?
// - a source ranked below cur: CLOCK_OUTRANKED
// - SNTP: clock_check_guarded(), "synced" meaning SNTP already set it this boot
// - MANUAL (#64): any plausible time, the user's choice, backwards too
// - APP: plausible, and forward only while the clock is plausible: MeshCore's boot-time bootstrap sets the newest
//   contact timestamp + 1, older than a clock the reset carried over (#82)
// g is only touched for SNTP.
clock_verdict_t clock_decide(clock_src_t cur, clock_src_t src, clock_guard_t *g, int64_t cand, int64_t now,
                             int64_t build_epoch, int64_t mono_s);

static inline bool clock_accepted(clock_verdict_t v) { return v == CLOCK_OK || v == CLOCK_RECOVERED; }

char const *clock_verdict_text(clock_verdict_t v);
// "SNTP", "set by hand", "from mesh contacts, unverified", "carried over the restart", "not set"
char const *clock_src_text(clock_src_t s);
// One line for "fw clock", About and Settings: clock_src_text(), for SNTP and by hand how long ago it set the clock
// (age_s): "SNTP, just now", "SNTP, 3 min ago", "set by hand, 2 h ago", "SNTP, 4 d ago"; for a carried clock the
// source before the reset (was, see clock_src_was()): "carried over the restart, was SNTP" (#64)
void clock_src_line(clock_src_t s, clock_src_t was, int64_t age_s, char *out, size_t cap);
// A word for the Settings row: "SNTP", "by hand", "mesh", "carried", "not set" (#64)
char const *clock_src_short(clock_src_t s);

// The source remembered from before a warm reset (#64): only for a carried clock (cur RTC), and only a real source
// (APP, MANUAL, SNTP); anything else, a garbled record included, gives NONE. A power-off clears the clock (no VBAT), so
// cur is NONE then and the record means nothing.
clock_src_t clock_src_was(clock_src_t cur, clock_src_t stored);
// Mesh time, unverified: the status bar's amber ~ (#64). Set by an app this boot, or carried over a reset from a clock
// an app had set. A carried clock without a record is shown plain: amber is only for mesh time.
bool clock_src_unverified(clock_src_t cur, clock_src_t was);
// "fw clock set --force" (#64, bench only): the rank a manual set is judged against, SNTP dropped to MANUAL so the
// manual path can be tried on a badge with Wi-Fi
clock_src_t clock_force_rank(clock_src_t cur);

#ifdef __cplusplus
}
#endif
