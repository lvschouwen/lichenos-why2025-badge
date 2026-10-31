#include "tz_table.h"

#include <string.h>

static tz_entry_t const zones[] = {
    {"UTC", "UTC0"},
    {"London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Amsterdam, Berlin, Paris", TZ_DEFAULT_POSIX},
    {"Helsinki, Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"New York", "EST5EDT,M3.2.0,M11.1.0"},
    {"Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"Los Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};

size_t tz_count(void) {
    return sizeof(zones) / sizeof(zones[0]);
}

tz_entry_t const *tz_at(size_t index) {
    return index < tz_count() ? &zones[index] : NULL;
}

// Zones that share a rule share an entry, so a posix string maps to exactly one
int tz_find_posix(char const *posix) {
    for (size_t i = 0; i < tz_count(); i++) {
        if (strcmp(zones[i].posix, posix) == 0) {
            return (int)i;
        }
    }
    return -1;
}
