#pragma once
// Files app (#71): the file work, plain POSIX so it is host-tested on a temp directory (files_ops_test.c). The
// protected-path rules of files_model.h are enforced here too, not only in the screens. Nothing in here maps flash (#31)
// or takes the SD card's lock: files_job.c wraps the calls in sd_begin/sd_end and runs them off the LVGL task.

#include "files_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FILES_OK,
    FILES_CANCELLED,
    FILES_ERR_ARG,       // a bad path or name
    FILES_ERR_PROTECTED, // the identity, keys, staged images and other OS files
    FILES_ERR_EXISTS,    // a copy never overwrites
    FILES_ERR_NOT_FILE,  // a copy takes plain files only
    FILES_ERR_NOT_EMPTY, // a delete takes files and empty directories only
    FILES_ERR_SPACE,
    FILES_ERR_OPEN,
    FILES_ERR_IO,
    FILES_ERR_NO_MEM,
    FILES_ERR_BUSY, // EACCES/EBUSY: another task has the file open (FatFs's lock #274, LittleFS #300), or read-only
    // A copy says which step failed (#443), so the screen can tell the three apart
    FILES_ERR_CARD, // the card is not ready or another user holds it (sd_begin refused)
    FILES_ERR_SRC,  // the source could not be examined or opened
    FILES_ERR_DEST, // the target file could not be created (FatFs: name not allowed, no directory entry, ...)
    FILES_ERR_NAME, // the name cannot exist on FAT: refused before anything is opened
} files_result_t;

// The step of a copy that failed, with the errno it left (0 when none was set)
typedef enum { FILES_STEP_NONE, FILES_STEP_SD_BEGIN, FILES_STEP_SRC_STAT, FILES_STEP_SRC_OPEN, FILES_STEP_DST_OPEN } files_step_t;
typedef struct {
    files_step_t step;
    int          err;
} files_fail_t;

char const *files_step_name(files_step_t step); // for the log line

char const *files_result_text(files_result_t r); // a sentence for the screen

// Lists `dir` into out (at most max), sorted. *count = entries stored; *skipped = entries left out (name too long, or
// no room). errno of the failed opendir otherwise, 0 on success.
int files_list(char const *dir, files_entry_t *out, size_t max, size_t *count, size_t *skipped);

// A file or an empty directory. Protected paths and the volume roots are refused.
files_result_t files_delete(char const *path);

// Reads up to cap bytes of the file from offset (fseek + fread); *got = bytes read (fewer at the end), *total = file
// size. Secret files (files_secret) are refused.
files_result_t files_read_at(char const *path, uint32_t offset, uint8_t *buf, size_t cap, size_t *got, uint32_t *total);

// Progress during a copy: bytes done of total; return false to cancel
typedef bool (*files_progress_fn)(uint32_t done, uint32_t total, void *ctx);

// Copies src to dst in chunks of buf. Never overwrites (FILES_ERR_EXISTS), refuses secret sources and protected targets (nothing is copied into a protected tree). The
// data goes to "<dst>.lichen-part" first and is renamed when complete; on failure or cancel the .part file is removed, so
// dst is either whole or absent. free_bytes: room on dst's volume (UINT64_MAX when unknown).
// A marker a user is unlikely to have; only a file with this suffix is ever removed as a leftover
#define FILES_PART_SUFFIX ".lichen-part"

// fail (may be NULL) is filled when the copy fails at one of the steps above.
files_result_t files_copy_ex(char const *src, char const *dst, uint8_t *buf, size_t buf_cap, uint64_t free_bytes,
                             files_progress_fn progress, void *ctx, files_fail_t *fail);
files_result_t files_copy(char const *src, char const *dst, uint8_t *buf, size_t buf_cap, uint64_t free_bytes,
                          files_progress_fn progress, void *ctx);

#ifdef __cplusplus
}
#endif
