// Host test: the time zone table Settings offers (#66)
#include "tz_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static int local_hour(char const *posix, time_t t) {
    setenv("TZ", posix, 1);
    tzset();
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour;
}

int main(void) {
    CHECK(tz_count() == 10);
    CHECK(tz_at(tz_count()) == NULL);

    int ams = tz_find_posix("CET-1CEST,M3.5.0,M10.5.0/3");
    CHECK(ams >= 0);
    if (ams < 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    CHECK(ams == tz_find_posix(TZ_DEFAULT_POSIX));
    CHECK(strncmp(tz_at((size_t)ams)->name, "Amsterdam", 9) == 0);
    CHECK(tz_find_posix("XYZ0") == -1);
    CHECK(tz_find_posix("") == -1);

    // Every entry has a name and a valid rule (UTC offset in January is within -12..+13 h of UTC)
    for (size_t i = 0; i < tz_count(); i++) {
        CHECK(tz_at(i)->name && tz_at(i)->posix);
        CHECK(tz_find_posix(tz_at(i)->posix) >= 0);
    }

    time_t july    = 1782907200; // 2026-07-01 12:00:00Z
    time_t january = 1767268800; // 2026-01-01 12:00:00Z
    CHECK(local_hour(tz_at((size_t)ams)->posix, july) == 14);
    CHECK(local_hour(tz_at((size_t)ams)->posix, january) == 13);
    CHECK(local_hour("UTC0", july) == 12);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("tz_table_test ok\n");
    return 0;
}
