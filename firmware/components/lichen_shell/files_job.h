#pragma once
// Files app (#71): one file job at a time on a short-lived task on core 0 (SD and flash I/O never run on the LVGL task).
// The screen starts a job, polls files_job_take() from an LVGL timer and reads the result; the job outlives the screen.

#include "files_ops.h"

#include <stdbool.h>
#include <stdint.h>

#define FILES_MAX_ENTRIES 256
#define FILES_READ_CHUNK  4096

typedef enum { FJ_ROOT, FJ_LIST, FJ_READ, FJ_DELETE, FJ_COPY } files_job_kind_t;

typedef struct {
    files_job_kind_t kind;
    files_result_t   result;
    int              err;        // errno of a failed listing
    // FJ_ROOT: both volumes; FJ_LIST: the listed directory's volume (free/total in the first slot)
    bool             have[2];
    uint64_t         free_bytes[2], total_bytes[2];
    // FJ_LIST: the entries live in files_job_entries()
    size_t           count, skipped;
    // FJ_READ
    size_t           got;
    uint32_t         file_size;
} files_job_result_t;

// Starts a job: path is the directory (ROOT: unused), the file (READ, DELETE) or the copy's source; for COPY dst is the
// target path, for READ offset is where to read. False when another job runs or the task could not start.
bool files_job_start(files_job_kind_t kind, char const *path, char const *dst, uint32_t offset);
// True once per finished job; fills *out
bool files_job_take(files_job_result_t *out);
bool files_job_busy(void);
// Asks the running copy to stop; it removes its partial file
void files_job_cancel(void);
// Bytes of the running copy so far
void files_job_progress(uint32_t *done, uint32_t *total);
// The last listing and the last text read (PSRAM; valid until the next job of that kind is started)
files_entry_t const *files_job_entries(void);
uint8_t const       *files_job_data(void);
