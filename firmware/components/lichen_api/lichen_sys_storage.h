#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The storage drive and the SD card's system surface (#227, #216 decision 1): the shell's About, Files, SD card,
// Power and notice code go through here, never to the sdcard and storage components. System surface: tools/layers.conf
// forbids lichen_sys_* in L6; an app reaches its own directory through lichen_fs.h. The types repeat sd_policy.h's
// (the gate cannot include a lower component's header); lichen_os checks that they stay equal.

// The mount points (#286): the shell and the gate name a drive by these, never by a literal. Equal to storage's
// STORAGE_BASE and sdcard's SD_BASE, which the gate cannot include (asserted in lichen_os, pinned by
// lichen_sys_pure_test). LICHEN_SYS_SD_LICHEN_DIR is Lichen's own directory on the card, sdcard's SD_LICHEN_DIR.
#define LICHEN_SYS_STORAGE_BASE  "/storage"
#define LICHEN_SYS_SD_BASE       "/sd"
#define LICHEN_SYS_SD_LICHEN_DIR LICHEN_SYS_SD_BASE "/lichen"

typedef enum {
    LICHEN_SD_OFF,     // never mounted, or unmounted by hand
    LICHEN_SD_NO_CARD, // the card did not answer (no card detect: this is the probe)
    LICHEN_SD_NOT_FAT, // a card, but no FAT on it
    LICHEN_SD_ERROR,   // a card that answered, then failed
    LICHEN_SD_MOUNTED,
    LICHEN_SD_REMOVED, // unmounted after repeated failed batches
} lichen_sd_state_t;

// How a batch of file work went: NEUTRAL failed, but not because of the card (full, a bad name, no memory)
typedef enum { LICHEN_SD_IO_OK, LICHEN_SD_IO_FAILED, LICHEN_SD_IO_NEUTRAL } lichen_sd_io_t;

// A copy of the card's cached values, never read from the card
typedef struct {
    lichen_sd_state_t state;
    uint64_t          total_bytes; // 0 when unknown
    uint64_t          free_bytes;
    bool              free_known;
    char              kind[6];     // "SDSC", "SDHC", "SDXC", "MMC"; "" when no card answered
    int               freq_khz;
    uint8_t           width;       // data lines
    uint32_t          io_errors;   // failed batches since the last mount
    uint32_t          generation;  // grows on every successful mount
    char              reason[32];  // why the last mount failed, for LICHEN_SD_ERROR
} lichen_sd_info_t;

// Cached state, sizes and card details; never touches the card. Any task, the LVGL task included.
void lichen_sys_sd_info(lichen_sd_info_t *out);
// Lock-free. Any task, the LVGL task included. A mounted card can still go away: file work goes between begin and end.
bool lichen_sys_sd_mounted(void);
// Powers the card, probes it and mounts its FAT; false when that fails (lichen_sys_sd_info says why). Never formats.
// Not on the LVGL task (it takes seconds).
bool lichen_sys_sd_mount(void);
// Waits for the current batch, unmounts and unpowers the card. Not on the LVGL task.
void lichen_sys_sd_unmount(void);
// Erases the card and makes one FAT volume over it, then mounts it. Not on the LVGL task.
bool lichen_sys_sd_format(void);
// One batch of file work under the card's mutex; false when the card is not mounted or the mutex is not free within
// wait_ms (then do no file work and do not call lichen_sys_sd_end). Not on the LVGL task: SD I/O can take a while.
bool lichen_sys_sd_begin(uint32_t wait_ms);
// Ends the batch and says how it went. Not on the LVGL task.
void lichen_sys_sd_end(lichen_sd_io_t result);
// 0 is OK; EIO, ENODEV and ETIMEDOUT are the card failing, any other errno is NEUTRAL. Any task.
lichen_sd_io_t lichen_sys_sd_io_from_errno(int err);
// "29.7 GB", "512 MB", "12 KB". Any task.
void lichen_sys_sd_fmt_size(uint64_t bytes, char *out, size_t cap);
// About's row: "28.1 GB free of 29.7 GB", "no card", ... Any task.
void lichen_sys_sd_short_text(lichen_sd_info_t const *info, char *out, size_t cap);
// The console's line: "sd mounted /sd 29.7 GB, ...", "sd no card". Any task.
void lichen_sys_sd_status_line(lichen_sd_info_t const *info, char *out, size_t cap);

// The boot mount of the storage drive worked. Any task, the LVGL task included.
bool lichen_sys_storage_mounted(void);
// Free and total bytes on the drive as last counted (at most about 10 min old: the OS counts every 10 min, #357, and
// again whenever the OTA receiver or "fw storage" needs a fresh figure); false when it is not mounted or the count is
// unknown. Never touches the drive: any task, the LVGL task included.
bool lichen_sys_storage_free_bytes(uint64_t *free_bytes, uint64_t *total_bytes);
