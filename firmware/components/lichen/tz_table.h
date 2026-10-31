#pragma once

#include <stddef.h>

// The time zones Settings offers (#66): a name and a POSIX TZ string (what setenv("TZ") takes, DST rules included)
typedef struct {
    char const *name;
    char const *posix;
} tz_entry_t;

#define TZ_DEFAULT_POSIX "CET-1CEST,M3.5.0,M10.5.0/3" // Amsterdam: the badge's home

size_t           tz_count(void);
tz_entry_t const *tz_at(size_t index);       // NULL when out of range
int              tz_find_posix(char const *posix); // index, or -1 if unknown
