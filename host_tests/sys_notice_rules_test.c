// Host test: which system notifications a snapshot of Lichen's getters raises (#62)
#include "sys_notice_rules.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static sysn_event_t ev[SYSN_EVENT_COUNT];

static size_t step(sysn_state_t *st, sysn_snapshot_t const *s) {
    return sysn_step(st, s, ev, SYSN_EVENT_COUNT);
}

static bool has(size_t n, sysn_event_t e) {
    for (size_t i = 0; i < n; i++) {
        if (ev[i] == e) {
            return true;
        }
    }
    return false;
}

static sysn_snapshot_t quiet(void) {
    sysn_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.storage_known = true;
    s.storage_total = 4u * 1024 * 1024;
    s.storage_free  = 2u * 1024 * 1024;
    return s;
}

static void test_boot_once(void) {
    sysn_state_t    st = {0};
    sysn_snapshot_t s  = quiet();
    s.crash_recovered  = true;
    s.psram_bad        = true;
    s.update           = SYSN_UPD_UPDATED;
    size_t n           = step(&st, &s);
    CHECK(n == 3 && has(n, SYSN_CRASH) && has(n, SYSN_PSRAM) && has(n, SYSN_UPDATED));
    CHECK(step(&st, &s) == 0); // the boot facts are told once
    sysn_state_t st2 = {0};
    s                = quiet();
    s.update         = SYSN_UPD_ROLLED_BACK;
    n                = step(&st2, &s);
    CHECK(n == 1 && ev[0] == SYSN_ROLLED_BACK); // a rollback, not an install
    st2 = (sysn_state_t){0};
    s   = quiet();
    CHECK(step(&st2, &s) == 0); // a quiet boot says nothing
}

static void test_counters(void) {
    sysn_state_t    st = {0};
    sysn_snapshot_t s  = quiet();
    s.c6_restarts      = 0;
    step(&st, &s);
    s.clock_refused = 1;
    size_t n        = step(&st, &s);
    CHECK(n == 1 && ev[0] == SYSN_CLOCK_IGNORED);
    CHECK(step(&st, &s) == 0);
    s.clock_recovered = true;
    CHECK(step(&st, &s) == 1 && ev[0] == SYSN_CLOCK_RECOVERED);
    CHECK(step(&st, &s) == 0);
    s.c6_restarts = 2;
    CHECK(step(&st, &s) == 1 && ev[0] == SYSN_C6_RESTARTED);
    CHECK(step(&st, &s) == 0);
}

static void test_sd(void) {
    sysn_state_t    st = {0};
    sysn_snapshot_t s  = quiet();
    s.sd_state         = SYSN_SDS_OFF;
    CHECK(step(&st, &s) == 0);
    s.sd_state = SYSN_SDS_MOUNTED; // the boot mount: normal, no notice
    CHECK(step(&st, &s) == 0);
    s.sd_state = SYSN_SDS_OFF; // fw sd unmount
    CHECK(step(&st, &s) == 1 && ev[0] == SYSN_SD_UNMOUNTED);
    s.sd_state = SYSN_SDS_MOUNTED;
    CHECK(step(&st, &s) == 1 && ev[0] == SYSN_SD_MOUNTED);
    s.sd_state = SYSN_SDS_REMOVED;
    CHECK(step(&st, &s) == 1 && ev[0] == SYSN_SD_REMOVED_ERRORS);
    s.sd_state = SYSN_SDS_NO_CARD;
    CHECK(step(&st, &s) == 0); // a mount attempt without a card: nothing lost
    s.sd_state = SYSN_SDS_OFF;
    CHECK(step(&st, &s) == 0); // OFF not from MOUNTED
    // A card Lichen can't use at boot is news; no card at boot is not
    sysn_state_t st2 = {0};
    s.sd_state       = SYSN_SDS_NOT_FAT;
    CHECK(step(&st2, &s) == 1 && ev[0] == SYSN_SD_NOT_FAT_CARD);
    sysn_state_t st3 = {0};
    s.sd_state       = SYSN_SDS_NO_CARD;
    CHECK(step(&st3, &s) == 0);
}

static void test_sensors(void) {
    sysn_state_t    st = {0};
    sysn_snapshot_t s  = quiet();
    s.sensors_running  = true;
    // A chip that never answered (not fitted) is no news
    for (int i = 0; i < 3 * SYSN_SENSOR_POLLS; i++) {
        CHECK(step(&st, &s) == 0);
    }
    s.imu_valid = true;
    s.env_valid = true;
    step(&st, &s);
    s.imu_valid = false;
    size_t n    = 0;
    for (int i = 0; i < SYSN_SENSOR_POLLS - 1; i++) {
        n += step(&st, &s);
    }
    CHECK(n == 0);
    n = step(&st, &s);
    CHECK(n == 1 && ev[0] == SYSN_IMU_LOST);
    for (int i = 0; i < 3 * SYSN_SENSOR_POLLS; i++) {
        CHECK(step(&st, &s) == 0); // once per boot
    }
    // A stopped service clears the flags: not a lost chip
    s.sensors_running = false;
    s.env_valid       = false;
    for (int i = 0; i < 3 * SYSN_SENSOR_POLLS; i++) {
        CHECK(step(&st, &s) == 0);
    }
}

static void test_storage(void) {
    sysn_state_t    st = {0};
    sysn_snapshot_t s  = quiet();
    step(&st, &s);
    s.storage_free = SYSN_LOW_BYTES - 1;
    CHECK(step(&st, &s) == 1 && ev[0] == SYSN_STORAGE_LOW);
    CHECK(step(&st, &s) == 0);
    s.storage_free = SYSN_LOW_BYTES + 100; // just above: still the same shortage (hysteresis)
    CHECK(step(&st, &s) == 0);
    s.storage_free = SYSN_LOW_BYTES - 1;
    CHECK(step(&st, &s) == 0);
    s.storage_free = 3u * 1024 * 1024; // room again, then short again: told again
    CHECK(step(&st, &s) == 0);
    s.storage_free = 100;
    CHECK(step(&st, &s) == 1);
    // Under 10 % of a big drive counts too; unknown says nothing
    sysn_state_t st2 = {0};
    s                = quiet();
    s.storage_total  = 100u * 1024 * 1024;
    s.storage_free   = 9u * 1024 * 1024;
    CHECK(step(&st2, &s) == 1 && ev[0] == SYSN_STORAGE_LOW);
    sysn_state_t st3 = {0};
    s.storage_known  = false;
    s.storage_free   = 0;
    CHECK(step(&st3, &s) == 0);
}

static void test_info(void) {
    CHECK(sysn_info(SYSN_CRASH).level == 2 && strcmp(sysn_info(SYSN_CRASH).target, "crash") == 0);
    CHECK(strcmp(sysn_info(SYSN_SD_UNMOUNTED).target, "sd") == 0);
    for (int e = 0; e < SYSN_EVENT_COUNT; e++) {
        CHECK(sysn_info((sysn_event_t)e).title && strlen(sysn_info((sysn_event_t)e).title) < 40);
    }
    // max caps the output
    sysn_state_t    st = {0};
    sysn_snapshot_t s  = quiet();
    s.crash_recovered  = true;
    s.psram_bad        = true;
    CHECK(sysn_step(&st, &s, ev, 1) == 1 && ev[0] == SYSN_CRASH);
}

int main(void) {
    test_boot_once();
    test_counters();
    test_sd();
    test_sensors();
    test_storage();
    test_info();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("sys_notice_rules_test: ok\n");
    return 0;
}
