// Lichen's own notifications (#62): see sys_notice_rules.h. Pure, host-tested in host_tests/sys_notice_rules_test.c.
#include "sys_notice_rules.h"

#define F_SILENT    0x01u // lichen_notify.h's flags
#define F_COALESCE  0x02u
#define F_ONCE_BOOT 0x04u

static bool storage_low(sysn_snapshot_t const *s, uint32_t factor) {
    return s->storage_known && s->storage_total &&
           (s->storage_free < (uint64_t)SYSN_LOW_BYTES * factor || s->storage_free * 10 < s->storage_total * factor);
}

static void sd_change(sysn_state_t *st, int now, sysn_event_t *out, size_t *n) {
    int was      = st->sd_state;
    st->sd_state = now;
    if (!st->sd_settled) {
        if (now == SYSN_SDS_OFF) {
            return; // still before the boot mount
        }
        st->sd_settled = true;
        // The boot mount: a mounted card or none is the normal case; a card Lichen cannot use is news
        if (now == SYSN_SDS_NOT_FAT) {
            out[(*n)++] = SYSN_SD_NOT_FAT_CARD;
        } else if (now == SYSN_SDS_ERROR) {
            out[(*n)++] = SYSN_SD_FAILED;
        }
        return;
    }
    if (now == was) {
        return;
    }
    switch (now) {
    case SYSN_SDS_MOUNTED: out[(*n)++] = SYSN_SD_MOUNTED; break;
    case SYSN_SDS_OFF: out[(*n)++] = was == SYSN_SDS_MOUNTED ? SYSN_SD_UNMOUNTED : SYSN_EVENT_COUNT; break;
    case SYSN_SDS_REMOVED: out[(*n)++] = SYSN_SD_REMOVED_ERRORS; break;
    case SYSN_SDS_NOT_FAT: out[(*n)++] = SYSN_SD_NOT_FAT_CARD; break;
    case SYSN_SDS_ERROR: out[(*n)++] = SYSN_SD_FAILED; break;
    default: break; // NO_CARD after a mount attempt: "fw sd" says so, nothing was lost
    }
    if (*n && out[*n - 1] == SYSN_EVENT_COUNT) {
        (*n)--;
    }
}

// A chip that answered and then gave nothing for SYSN_SENSOR_POLLS polls while the service runs: once per boot
static bool sensor_lost(bool running, bool valid, bool *seen, uint32_t *missing, bool *told) {
    if (!running || valid) {
        *seen    = *seen || (running && valid);
        *missing = 0;
        return false;
    }
    if (!*seen || *told || ++*missing < SYSN_SENSOR_POLLS) {
        return false;
    }
    *told = true;
    return true;
}

size_t sysn_step(sysn_state_t *st, sysn_snapshot_t const *s, sysn_event_t *out, size_t max) {
    sysn_event_t ev[SYSN_EVENT_COUNT];
    size_t       n = 0;
    if (!st->started) {
        st->started     = true;
        st->c6_restarts = s->c6_restarts;
        st->sd_state    = SYSN_SDS_OFF;
        if (s->crash_recovered) {
            ev[n++] = SYSN_CRASH;
        }
        if (s->psram_bad) {
            ev[n++] = SYSN_PSRAM;
        }
        if (s->update == SYSN_UPD_UPDATED) {
            ev[n++] = SYSN_UPDATED;
        } else if (s->update == SYSN_UPD_ROLLED_BACK) {
            ev[n++] = SYSN_ROLLED_BACK;
        }
    }
    if (s->clock_refused > st->clock_refused) {
        ev[n++] = SYSN_CLOCK_IGNORED;
    }
    st->clock_refused = s->clock_refused;
    if (s->clock_recovered && !st->clock_recovered) {
        ev[n++] = SYSN_CLOCK_RECOVERED;
    }
    st->clock_recovered = s->clock_recovered;
    if (s->c6_restarts > st->c6_restarts) {
        ev[n++] = SYSN_C6_RESTARTED;
    }
    st->c6_restarts = s->c6_restarts;
    sd_change(st, s->sd_state, ev, &n);
    if (sensor_lost(s->sensors_running, s->imu_valid, &st->imu_seen, &st->imu_missing, &st->imu_told)) {
        ev[n++] = SYSN_IMU_LOST;
    }
    if (sensor_lost(s->sensors_running, s->env_valid, &st->env_seen, &st->env_missing, &st->env_told)) {
        ev[n++] = SYSN_ENV_LOST;
    }
    if (!st->storage_low && storage_low(s, 1)) {
        st->storage_low = true;
        ev[n++]         = SYSN_STORAGE_LOW;
    } else if (st->storage_low && s->storage_known && !storage_low(s, 2)) {
        st->storage_low = false; // room again: a later shortage is news again
    }
    size_t k = 0;
    for (; k < n && k < max; k++) {
        out[k] = ev[k];
    }
    return k;
}

sysn_info_t sysn_info(sysn_event_t event) {
    static sysn_info_t const table[SYSN_EVENT_COUNT] = {
        [SYSN_CRASH]             = {2, F_ONCE_BOOT, "crash", "Restarted after a crash"},
        [SYSN_PSRAM]             = {1, F_ONCE_BOOT, "psram", "PSRAM check failed at boot"},
        [SYSN_CLOCK_IGNORED]     = {1, F_ONCE_BOOT, "clock", "Clock answer ignored"},
        [SYSN_CLOCK_RECOVERED]   = {0, F_ONCE_BOOT, "clock", "Clock corrected"},
        [SYSN_UPDATED]           = {0, F_ONCE_BOOT, "update", "Lichen updated"},
        [SYSN_ROLLED_BACK]       = {1, F_ONCE_BOOT, "update", "Update failed"},
        [SYSN_C6_RESTARTED]      = {1, F_COALESCE, "c6", "Radio chip (C6) restarted"},
        [SYSN_SD_MOUNTED]        = {0, F_COALESCE, "sd", "SD card mounted"},
        [SYSN_SD_UNMOUNTED]      = {0, F_COALESCE, "sd", "SD card unmounted"},
        [SYSN_SD_REMOVED_ERRORS] = {1, F_COALESCE, "sd", "SD card removed"},
        [SYSN_SD_NOT_FAT_CARD]   = {1, F_COALESCE, "sd", "SD card not usable"},
        [SYSN_SD_FAILED]         = {1, F_COALESCE, "sd", "SD card error"},
        [SYSN_IMU_LOST]          = {1, F_ONCE_BOOT, "sensors", "Motion sensor stopped"},
        [SYSN_ENV_LOST]          = {1, F_ONCE_BOOT, "sensors_env", "Air sensor stopped"},
        [SYSN_STORAGE_LOW]       = {1, F_COALESCE, "storage", "Storage almost full"},
    };
    (void)F_SILENT;
    return event < SYSN_EVENT_COUNT ? table[event] : table[SYSN_CRASH];
}
