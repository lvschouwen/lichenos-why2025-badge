#pragma once
// The badge's microSD card (#60), FAT at SD_BASE. OS code only: apps reach their own directory through lichen_fs.h.
//
// Hardware: SDMMC slot 0 on its IO-MUX pins (D0-D3 = GPIO39-42, CLK 43, CMD 44), 4-bit, 20 MHz, powered by the
// on-chip LDO channel 4 (VO4 feeds VDD_IO_5 and the card's VDD). No card detect: DET and GPIO46 are on GND on
// production boards, so a card is found by probing at mount and GPIO46 is never touched. The C6 link (esp_hosted)
// shares the SDMMC controller on slot 1; each side only ever deinits its own slot.

#include "sd_policy.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_BASE         "/sd"
#define SD_LICHEN_DIR   SD_BASE "/lichen"      // Lichen's own files; made at mount. Anything else on the card is left alone
#define SD_APPS_DIR     SD_LICHEN_DIR "/apps"   // lichen_fs_sd_dir(): one directory per app
#define SD_MESSAGES_DIR SD_LICHEN_DIR "/messages" // MeshCore's message history (#48)

// Registers "fw sd" and mounts the card once. A missing or unusable card is one ESP_LOGW; nothing else depends on it.
void sd_start(void);

// Powers the card, probes it and mounts its FAT. False when there is no card, no FAT or an error (sd_info says
// which); the card is unpowered again then. Never formats.
bool sd_mount(void);
// Waits for the current batch (sd_begin/sd_end), unmounts and unpowers the card
void sd_unmount(void);
// Lock-free, safe from any task including LVGL. A mounted card can still go away between this and the next access:
// OS code does its file work between sd_begin and sd_end instead.
bool sd_mounted(void);

// One batch of file work (open, write, close) under the card's mutex, so the card is never unmounted under an open
// FILE*. False when the card is not mounted or the mutex is not free within wait_ms: then do no file work and do
// not call sd_end. sd_end says how the batch went: SD_IO_FAILED only when the card itself failed (errno EIO, ENODEV,
// ETIMEDOUT: use sd_io_from_errno); a full card, a bad name or out of memory is SD_IO_NEUTRAL. SD_IO_ERRORS_TO_UNMOUNT
// failed batches in a row unmount the card (SD_STATE_REMOVED) until "fw sd mount" or a reboot. Never from the LVGL
// task: SD I/O can take a while.
bool sd_begin(uint32_t wait_ms);
void sd_end(sd_io_t result);

// Cached state, sizes and card details; never touches the card, safe from LVGL. Free space is refreshed at mount,
// after every sd_end and by "fw sd".
void sd_info(sd_info_t *out);

// "fw sd format": erases the card and makes one FAT volume over all of it (FAT32 from about 256 MB up, cluster size
// chosen by FatFs from the card size, label LICHEN), then mounts it. Waits for the current batch first.
bool sd_format(void);

#ifdef __cplusplus
}
#endif
