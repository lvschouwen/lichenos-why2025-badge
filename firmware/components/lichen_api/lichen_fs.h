#pragma once
// An app's files (#84 T6): one directory per app on the storage drive, "/storage/<id>", made on first use. Everything
// else on the drive is the OS's (the C6 image under /storage/c6, lora.toml, the OTA token): an app never names a path
// outside its own directory, and its file shim refuses ".." (fs_shim.cpp). With a card in, an app also gets one
// directory on the SD card (#60), "/sd/lichen/apps/<id>"; the rest of the card is the OS's or the user's.

#include "lichen_app.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// "/storage/<id>", valid for as long as the badge runs; NULL (logged) when the id is unusable or the directory cannot
// be made (no drive)
char const *lichen_fs_app_dir(lichen_app_t const *app);

// The drive the app directories live on: false (zeros) when it is not mounted
bool lichen_fs_drive_bytes(size_t *total, size_t *used);

// "/sd/lichen/apps/<id>", made on first use after each mount; NULL when no card is mounted (normal, not logged), the
// id is unusable or the directory cannot be made (logged). Ask again each time instead of keeping the path across a
// card change: it is cheap. Not from the LVGL task (the first call per mount writes to the card). The card can be
// pulled at any time: every file call can fail, and an open file is dead after "fw sd unmount".
char const *lichen_fs_sd_dir(lichen_app_t const *app);

// The SD card's FAT volume, from values cached at mount and after each write batch (never reads the card, safe from
// LVGL): false (zeros) when no card is mounted. 64-bit: a card is larger than size_t.
bool lichen_fs_sd_bytes(uint64_t *total, uint64_t *used);


// One batch of file work on the SD card (open, write, close) under the card's lock, for an app that keeps its own files
// outside lichen_fs_sd_dir (MeshCore's message journal, #48). False when no card is mounted or the lock is not free
// within wait_ms (then do no file work and do not call lichen_fs_sd_end; the log names the app that holds it). Never
// from the LVGL task. The card can fail or be pulled at any time: end the batch with what happened.
bool lichen_fs_sd_begin(lichen_app_t const *app, uint32_t wait_ms);
// err is 0 when the batch worked, else the errno of the failing call: only a card that fails (EIO, ENODEV, ETIMEDOUT)
// counts toward the auto-unmount, not a full card or a bad name
void lichen_fs_sd_end(lichen_app_t const *app, int err);
// Lock-free, any task: whether a card is mounted right now (it can still go away before the batch starts)
bool lichen_fs_sd_mounted(void);

#ifdef __cplusplus
}
#endif
