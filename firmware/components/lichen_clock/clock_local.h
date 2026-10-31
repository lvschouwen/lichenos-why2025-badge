#pragma once
// A local date and time, as a person enters it, to UTC in the selected zone (#64). Pure: uses the C library's TZ
// (settings_apply_tz() on the badge, setenv + tzset in host tests) through localtime_r only. Host-tested.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// GAP: the time does not exist (spring forward, 02:30 in Amsterdam): read with the offset before the change, so it is
// 03:30 summer time. TWICE: it exists twice (autumn, 02:30): the first instance, summer time, is used. Both are
// accepted; the caller shows a hint. BAD_DATE: 30 February, hour 24 and the like; *utc is left alone.
typedef enum { CLOCK_LOCAL_OK, CLOCK_LOCAL_GAP, CLOCK_LOCAL_TWICE, CLOCK_LOCAL_BAD_DATE } clock_local_t;

int           clock_days_in_month(int year, int month);
clock_local_t clock_local_to_utc(int year, int month, int day, int hour, int minute, int64_t *utc);

// "fw clock set": YYYY-MM-DD[T| ]HH:MM[:SS] followed by Z, +HH:MM / -HH:MM (+HHMM too), or nothing (local in the
// selected zone, through clock_local_to_utc; *how says GAP/TWICE). Spaces around are ignored. false for anything else.
// how may be NULL; it is OK for an explicit offset.
bool clock_parse_iso(char const *s, int64_t *utc, clock_local_t *how);

#define CLOCK_BACK_HINT_S (5 * 60)
// The hints Set date & time and "fw clock set" show, NULL when there is nothing to say. clock_local_hint: GAP and
// TWICE. clock_back_hint: the new time is more than 5 min behind the clock: MeshCore nodes keep the newest advert
// they saw from us, so ours are ignored until the clock is past that time again.
char const *clock_local_hint(clock_local_t how);
char const *clock_back_hint(int64_t now, int64_t set_to);

#ifdef __cplusplus
}
#endif
