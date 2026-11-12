#pragma once
// Lichen's own notifications (#62): which events a once-a-second snapshot of the system's getters raises. Pure (snapshot
// in, events out), host-tested in host_tests/sys_notice_rules_test.c; sys_notices.c reads the getters and raises them.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// LICHEN_SD_* values (lichen_sys_storage.h), repeated so this stays free of the SD headers
enum { SYSN_SDS_OFF, SYSN_SDS_NO_CARD, SYSN_SDS_NOT_FAT, SYSN_SDS_ERROR, SYSN_SDS_MOUNTED, SYSN_SDS_REMOVED };
enum { SYSN_UPD_NONE, SYSN_UPD_UPDATED, SYSN_UPD_ROLLED_BACK }; // upd_result_t

typedef struct {
    bool     crash_recovered; // this boot follows a crash (#72)
    bool     psram_bad;       // the PSRAM tuning or boot check failed (#40)
    bool     psram_fell;      // PSRAM evidence at 120 MHz put the badge back at 80 until a speed is chosen (#353)
    uint32_t clock_refused;   // time answers refused since boot (#8)
    bool     clock_recovered; // the last answer was taken as a recovery from a wrong clock
    int      update;          // SYSN_UPD_*: the boot after an update (#76)
    uint32_t c6_restarts;     // C6 outages it came back from (#14)
    int      sd_state;        // SYSN_SDS_* (#60)
    bool     sensors_running;
    bool     imu_valid;
    bool     env_valid;
    bool     storage_unmounted; // the boot mount failed (#110): the drive is left as it is, not formatted
    bool     storage_known;
    uint64_t storage_free;
    uint64_t storage_total;
} sysn_snapshot_t;

typedef enum {
    SYSN_CRASH,
    SYSN_PSRAM,
    SYSN_CLOCK_IGNORED,
    SYSN_CLOCK_RECOVERED,
    SYSN_UPDATED,
    SYSN_ROLLED_BACK,
    SYSN_C6_RESTARTED,
    SYSN_SD_MOUNTED,
    SYSN_SD_UNMOUNTED,
    SYSN_SD_REMOVED_ERRORS,
    SYSN_SD_NOT_FAT_CARD,
    SYSN_SD_FAILED,
    SYSN_IMU_LOST,
    SYSN_ENV_LOST,
    SYSN_STORAGE_LOW,
    SYSN_STORAGE_UNMOUNTED,
    SYSN_PSRAM_80,
    SYSN_EVENT_COUNT
} sysn_event_t;

#define SYSN_SENSOR_POLLS 10 // a chip that answered and then gave nothing for this many polls stopped answering
// Low storage (Lucas, #62 Q6): under 10 % or under 512 KB free; told again only after it rose above twice that
#define SYSN_LOW_BYTES (512u * 1024u)

typedef struct {
    bool     started;
    uint32_t clock_refused;
    bool     clock_recovered;
    uint32_t c6_restarts;
    int      sd_state;
    bool     sd_settled; // the boot mount is over (the SD left OFF once): changes from now on are news
    bool     imu_seen, env_seen;
    uint32_t imu_missing, env_missing;
    bool     imu_told, env_told;
    bool     storage_low;
} sysn_state_t;

// One poll: up to max events in out (SYSN_EVENT_COUNT is always enough); returns how many. state starts zeroed.
size_t sysn_step(sysn_state_t *state, sysn_snapshot_t const *snap, sysn_event_t *out, size_t max);

typedef struct {
    uint8_t     level;  // ln_level_t
    uint32_t    flags;  // lichen_notify flags
    char const *target; // the place key: "crash" opens the Crash log, "sd" Settings › SD card
    char const *title;
} sysn_info_t;
sysn_info_t sysn_info(sysn_event_t event);

// ---- The poll's own cost (#357) ----
// The free-space count is LittleFS's lfs_fs_size, a walk over every block in use. Its caches are in PSRAM, so each
// flash read of the walk goes through a temporary internal buffer that spi_flash sizes with
// heap_caps_get_largest_free_block: a walk of every internal heap block inside a critical section, per read. The IDLE0
// watchdog every ~66 s at 5 h of uptime suggests one count then took over 5 s of core 0 (inferred from the watchdog's
// samples, not yet timed on the bench). Once every 10 min; boot (poll 0) counts at once. Between counts the snapshot
// takes the last count anyone made (storage_free_bytes_cached: OTA, "fw storage").
#define SYSN_STORAGE_POLLS 600u
bool sysn_storage_due(unsigned poll);

typedef enum { SYSN_STEP_GETTERS, SYSN_STEP_SD, SYSN_STEP_SENSORS, SYSN_STEP_STORAGE, SYSN_STEP_COUNT } sysn_step_id_t;

#define SYSN_SLOW_MS     500u                // a step this long is a warning
#define SYSN_SLOW_GAP_MS (30u * 60u * 1000u) // per step: one warning, then the next after this long at the least

typedef struct {
    uint32_t max_ms[SYSN_STEP_COUNT]; // the longest since boot
    uint32_t untold[SYSN_STEP_COUNT]; // slow ones since the last warning (that one included)
    uint32_t untold_max[SYSN_STEP_COUNT];
    uint64_t told_at_ms[SYSN_STEP_COUNT];
    bool     told[SYSN_STEP_COUNT];
} sysn_timing_t;

typedef struct {
    uint32_t count;  // slow ones this warning covers (the one now included)
    uint32_t max_ms; // the longest of them
} sysn_slow_t;

// Records one step's time; true when it is to be logged as a warning now (*out: what the warning covers). The state
// starts zeroed.
bool        sysn_time_step(sysn_timing_t *t, sysn_step_id_t step, uint32_t took_ms, uint64_t now_ms, sysn_slow_t *out);
char const *sysn_step_name(sysn_step_id_t step);

#ifdef __cplusplus
}
#endif
