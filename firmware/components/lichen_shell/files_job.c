// Files app (#71): see files_job.h. A job takes the SD card's batch lock (sd_begin/sd_end) when a path is on /sd, so the
// card is not unmounted under an open file; the copy holds it for the whole file and checks the cancel flag per chunk.
#include "files_job.h"

#include "lichen_mem.h"
#include "lichen_sys_app.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_sys_storage.h"
#include "lichen_task.h"

#include <errno.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

static char const TAG[] = "files";

#define JOB_STACK    6144
#define COPY_CHUNK   (32 * 1024)
#define SD_WAIT_MS   3000
enum { IDLE, RUNNING, DONE };

static atomic_int  s_state;
static atomic_bool s_cancel;
static atomic_uint s_done, s_total;

EXT_RAM_BSS_ATTR static struct {
    files_job_kind_t kind;
    char             path[FILES_PATH_MAX], dst[FILES_PATH_MAX];
    uint32_t         offset;
} s_req;
static files_job_result_t s_result;
static files_entry_t     *s_entries; // PSRAM, FILES_MAX_ENTRIES; kept for the next job
static uint8_t           *s_data;    // PSRAM, FILES_READ_CHUNK

static bool on_sd(char const *path) {
    return path[0] && files_volume_of(path) && !strcmp(files_volume_of(path), FILES_SD);
}

static void volume_space(int slot, bool sd) {
    uint64_t free_bytes = 0, total_bytes = 0;
    bool     ok;
    if (sd) {
        lichen_sd_info_t info;
        lichen_sys_sd_info(&info);
        ok          = info.state == LICHEN_SD_MOUNTED;
        free_bytes  = info.free_bytes;
        total_bytes = info.total_bytes;
    } else {
        ok = lichen_sys_storage_free_bytes(&free_bytes, &total_bytes);
    }
    s_result.have[slot]        = ok;
    s_result.free_bytes[slot]  = ok ? free_bytes : 0;
    s_result.total_bytes[slot] = ok ? total_bytes : 0;
}

static bool copy_progress(uint32_t done, uint32_t total, void *ctx) {
    (void)ctx;
    atomic_store(&s_done, done);
    atomic_store(&s_total, total);
    return !atomic_load(&s_cancel);
}

static files_result_t run_copy(void) {
    uint8_t *buf = lichen_alloc(lichen_sys_ctx_os(), COPY_CHUNK, LICHEN_MEM_BULK);
    if (!buf) {
        return FILES_ERR_NO_MEM;
    }
    uint64_t free_bytes = UINT64_MAX;
    int      slot       = on_sd(s_req.dst) ? 1 : 0;
    volume_space(slot, slot == 1);
    if (s_result.have[slot]) {
        free_bytes = s_result.free_bytes[slot];
    }
    files_fail_t   fail = {FILES_STEP_NONE, 0};
    files_result_t r    = files_copy_ex(s_req.path, s_req.dst, buf, COPY_CHUNK, free_bytes, copy_progress, NULL, &fail);
    s_result.step       = fail.step;
    s_result.err        = fail.err;
    lichen_free(lichen_sys_ctx_os(), buf);
    return r;
}

static files_result_t run_list(void) {
    if (!s_entries) {
        s_entries = lichen_alloc(lichen_sys_ctx_os(), FILES_MAX_ENTRIES * sizeof(*s_entries), LICHEN_MEM_BULK);
    }
    if (!s_entries) {
        return FILES_ERR_NO_MEM;
    }
    s_result.err = files_list(s_req.path, s_entries, FILES_MAX_ENTRIES, &s_result.count, &s_result.skipped);
    volume_space(0, on_sd(s_req.path));
    return s_result.err ? FILES_ERR_OPEN : FILES_OK;
}

static files_result_t run_read(void) {
    if (!s_data) {
        s_data = lichen_alloc(lichen_sys_ctx_os(), FILES_READ_CHUNK, LICHEN_MEM_BULK);
    }
    return s_data ? files_read_at(s_req.path, s_req.offset, s_data, FILES_READ_CHUNK, &s_result.got, &s_result.file_size)
                  : FILES_ERR_NO_MEM;
}

static files_result_t run_kind(void) {
    switch (s_req.kind) {
    case FJ_ROOT:
        volume_space(0, false);
        volume_space(1, true);
        return FILES_OK;
    case FJ_LIST:   return run_list();
    case FJ_READ:   return run_read();
    case FJ_DELETE: return files_delete(s_req.path);
    case FJ_COPY:   return run_copy();
    }
    return FILES_ERR_ARG;
}

static void job_task(void *arg) {
    (void)arg;
    memset(&s_result, 0, sizeof(s_result));
    s_result.kind = s_req.kind;
    bool need_sd  = on_sd(s_req.path) || on_sd(s_req.dst);
    bool locked   = need_sd && lichen_sys_sd_begin(SD_WAIT_MS);
    if (need_sd && !locked) {
        s_result.result = FILES_ERR_CARD; // no card, or the card is busy
        s_result.step   = FILES_STEP_SD_BEGIN;
    } else {
        errno           = 0;
        s_result.result = run_kind();
    }
    if (locked) {
        // A copy recorded the errno of its failed step: the global one has been overwritten by the closes since
        int  err        = s_req.kind == FJ_COPY && s_result.step != FILES_STEP_NONE ? s_result.err : errno;
        bool card_error = files_card_strike(s_result.result, err);
        lichen_sys_sd_end(card_error ? lichen_sys_sd_io_from_errno(err) : LICHEN_SD_IO_OK);
    }
    if (s_req.kind == FJ_COPY && s_result.result != FILES_OK && s_result.step != FILES_STEP_NONE) {
        ESP_LOGI(TAG, "copy: step=%s errno=%d", files_step_name(s_result.step), s_result.err);
    }
    ESP_LOGI(TAG, "job %d -> %d, stack margin %u", (int)s_req.kind, (int)s_result.result,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    atomic_store(&s_state, DONE);
}

bool files_job_start(files_job_kind_t kind, char const *path, char const *dst, uint32_t offset) {
    int expected = IDLE;
    if (!atomic_compare_exchange_strong(&s_state, &expected, RUNNING)) {
        return false;
    }
    s_req.kind   = kind;
    s_req.offset = offset;
    snprintf(s_req.path, sizeof(s_req.path), "%s", path ? path : "");
    snprintf(s_req.dst, sizeof(s_req.dst), "%s", dst ? dst : "");
    atomic_store(&s_cancel, false);
    atomic_store(&s_done, 0);
    atomic_store(&s_total, 0);
    if (!lichen_task_start(lichen_ctx_self(), "filesjob", job_task, NULL, JOB_STACK, LICHEN_PRIO_JOB, NULL)) {
        ESP_LOGW(TAG, "no memory for a file job");
        atomic_store(&s_state, IDLE);
        return false;
    }
    return true;
}

bool files_job_take(files_job_result_t *out) {
    if (atomic_load(&s_state) != DONE) {
        return false;
    }
    *out = s_result;
    atomic_store(&s_state, IDLE);
    return true;
}

bool files_job_busy(void) {
    return atomic_load(&s_state) == RUNNING;
}

void files_job_cancel(void) {
    atomic_store(&s_cancel, true);
}

void files_job_progress(uint32_t *done, uint32_t *total) {
    *done  = atomic_load(&s_done);
    *total = atomic_load(&s_total);
}

files_entry_t const *files_job_entries(void) {
    return s_entries;
}

uint8_t const *files_job_data(void) {
    return s_data;
}
