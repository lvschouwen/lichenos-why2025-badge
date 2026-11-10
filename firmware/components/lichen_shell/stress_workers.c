// The Stress app's load (#278): five workers on the OS's tasks, one LVGL timer and a monitor that samples every 10 s,
// judges the run (stress_calc.c) and writes the report. Workers: stress-dma (DMA gate rounds through psram_guard plus a
// data-verified 1 MB PSRAM copy), stress-sd (64 KB write, read back, compare, delete on the card, paced so a run writes
// at most 64 MB), stress-fs (a 512 B file on /storage every 10 s), stress-lat (a 10 ms probe: how late it wakes) and the
// UI timer (a full redraw of the screen on top every 100 ms). LoRa is never touched: nothing is sent. Every line goes
// out through ESP_LOG (tag "stress"), so the UART and the log store see the same lines however the run was started.
#include "stress_workers.h"

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "files_model.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichen_mem.h"
#include "lichen_sys_mem.h"
#include "lichen_sys_storage.h"
#include "lichen_sys_ui.h"
#include "lichen_task.h"
#include "psram_dma_cmd.h"
#include "psram_speed.h"
#include "shell_parts.h"
#include "update_screen.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char const TAG[] = "stress";

#define SD_DIR        LICHEN_SYS_SD_LICHEN_DIR "/stress"
#define SD_FILE       SD_DIR "/stress.bin"
#define FS_FILE       FILES_STORAGE "/stress.tmp"
#define SD_CHUNK      (64u * 1024)
#define SD_CAP_KB     (64u * 1024) // 64 MB of writes per run at most: the card's wear stays negligible
#define SD_WAIT_MS    2000
#define FS_BYTES      512
#define FS_PERIOD_MS  10000
#define COPY_BYTES    (1024u * 1024)
#define CACHE_LINE    128
#define LAT_PERIOD_MS 10
#define SAMPLE_S      10
#define PSRAM_CHECK_EVERY 6 // samples: the PSRAM heap walk holds its lock longest, once a minute is enough
#define UI_TICK_MS    100
#define STACKS_MAX    64
#define FLUSH         (ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE)

typedef struct {
    _Atomic uint32_t dma_rounds, dma_busy, dma_bad, copies, mismatches, alloc_fails;
    _Atomic uint32_t sd_cycles, sd_kb, sd_io, fs_cycles, fs_io, frames;
    _Atomic uint32_t lat_worst_ms; // since the last sample
    atomic_bool      stop;         // every worker ends its loop
    atomic_int       workers;      // still running
} shared_t;

static shared_t      s_w;                // internal: workers on internal stacks read it with the cache off (flash writes)
static atomic_bool   s_on;               // a monitor task exists
static atomic_int    s_stop_req;         // the first stress_stop reason, STRESS_NONE when none
static TaskHandle_t  s_mon;
static portMUX_TYPE  s_view_mux = portMUX_INITIALIZER_UNLOCKED;
static stress_view_t s_view;             // under s_view_mux, internal (a critical section never touches PSRAM)
static lv_timer_t   *s_ui_timer;         // LVGL task only
static uint32_t      s_sd_pace_ms;       // the SD worker's gap between cycles for this run

typedef struct {
    lichen_sys_stack_t now[STACKS_MAX], start[STACKS_MAX];
    size_t             n_now, n_start;
} stacks_t;
EXT_RAM_BSS_ATTR static stacks_t s_stacks; // the monitor's only

// ---- patterns and files ---------------------------------------------------------------------------------------------

static void fill(uint8_t *buf, size_t n, uint32_t seed) {
    uint32_t *w = (uint32_t *)buf, x = seed | 1u;
    for (size_t i = 0; i < n / 4; i++) {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5; // xorshift32: every word differs, a stuck bit shows
        w[i] = x;
    }
}

typedef enum { FILE_OK, FILE_IO, FILE_MISMATCH } file_result_t;

static int write_all(char const *path, uint8_t const *a, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return errno ? errno : EIO;
    }
    int err = write(fd, a, n) == (ssize_t)n ? 0 : (errno ? errno : EIO);
    if (close(fd) != 0 && !err) {
        err = errno ? errno : EIO;
    }
    return err;
}

static int read_all(char const *path, uint8_t *b, size_t n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return errno ? errno : EIO;
    }
    int err = read(fd, b, n) == (ssize_t)n ? 0 : (errno ? errno : EIO);
    close(fd);
    return err;
}

// Writes a, reads it back into b, compares, deletes the file. *err gets the errno of a failed call.
static file_result_t file_round(char const *path, uint8_t const *a, uint8_t *b, size_t n, int *err) {
    errno = 0;
    *err  = write_all(path, a, n);
    if (!*err) {
        memset(b, 0, n);
        *err = read_all(path, b, n);
    }
    unlink(path);
    if (*err) {
        return FILE_IO;
    }
    return memcmp(a, b, n) == 0 ? FILE_OK : FILE_MISMATCH;
}

static void count_file(file_result_t r, int err, _Atomic uint32_t *cycles, _Atomic uint32_t *io, char const *what) {
    if (r == FILE_IO) {
        atomic_fetch_add(io, 1);
        ESP_LOGW(TAG, "%s: file error %d", what, err);
    } else if (r == FILE_MISMATCH) {
        atomic_fetch_add(&s_w.mismatches, 1);
        ESP_LOGE(TAG, "%s: read back different from what was written", what);
    } else {
        atomic_fetch_add(cycles, 1);
    }
}

// The test files of this run and of one that never got to clean up (a power cut). SD work only between begin and end.
static void cleanup_files(void) {
    unlink(FS_FILE);
    if (lichen_sys_sd_mounted() && lichen_sys_sd_begin(SD_WAIT_MS)) {
        unlink(SD_FILE);
        rmdir(SD_DIR);
        lichen_sys_sd_end(LICHEN_SD_IO_OK);
    }
}

// ---- the workers ----------------------------------------------------------------------------------------------------

static bool stopping(void) {
    return atomic_load(&s_w.stop);
}

// Sleeps ms in short steps so a stop is seen within 100 ms
static void nap(uint32_t ms) {
    for (uint32_t t = 0; t < ms && !stopping(); t += 100) {
        vTaskDelay(pdMS_TO_TICKS(ms - t < 100 ? ms - t : 100));
    }
}

static void worker_end(void) {
    atomic_fetch_sub(&s_w.workers, 1);
    xTaskNotifyGive(s_mon);
}

static void dma_round(void) {
    psram_dma_round_info_t info;
    switch (psram_dma_quick_round(&info)) {
    case PSRAM_DMA_ROUND_PASS: atomic_fetch_add(&s_w.dma_rounds, 1); break;
    case PSRAM_DMA_ROUND_FAIL: {
        atomic_fetch_add(&s_w.dma_rounds, 1);
        uint32_t bad = info.bad_bytes + info.errors;
        atomic_fetch_add(&s_w.dma_bad, bad ? bad : 1); // a failed round always counts
        ESP_LOGE(TAG, "dma round failed: %u bad bytes, %u errors", (unsigned)info.bad_bytes, info.errors);
        break;
    }
    case PSRAM_DMA_ROUND_BUSY: // never a pass: another gate holds the engine (#278)
        atomic_fetch_add(&s_w.dma_busy, 1);
        ESP_LOGW(TAG, "skip dma round: another DMA gate is running");
        nap(1000);
        break;
    case PSRAM_DMA_ROUND_NOT_RUN:
        atomic_fetch_add(&s_w.alloc_fails, 1);
        ESP_LOGE(TAG, "dma round not run: no memory for its buffers or no AXI-GDMA");
        break;
    }
}

static bool check(uint8_t const *buf, size_t n, uint32_t seed) {
    uint32_t const *w = (uint32_t const *)buf;
    uint32_t        x = seed | 1u;
    for (size_t i = 0; i < n / 4; i++) {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        if (w[i] != x) {
            return false;
        }
    }
    return true;
}

// 1 MB PSRAM to PSRAM by the CPU, both buffers pushed out of the cache, so the check reads what reached the RAM
static void copy_round(uint8_t *a, uint8_t *b, uint32_t seed) {
    fill(a, COPY_BYTES, seed);
    esp_cache_msync(a, COPY_BYTES, FLUSH);
    memcpy(b, a, COPY_BYTES);
    esp_cache_msync(b, COPY_BYTES, FLUSH);
    if (check(b, COPY_BYTES, seed)) {
        atomic_fetch_add(&s_w.copies, 1);
    } else {
        atomic_fetch_add(&s_w.mismatches, 1);
        ESP_LOGE(TAG, "psram copy: 1 MB read back different from the pattern");
    }
}

static void dma_worker(void *arg) {
    (void)arg;
    uint8_t *a = lichen_alloc_aligned(NULL, CACHE_LINE, COPY_BYTES, LICHEN_MEM_BULK);
    uint8_t *b = lichen_alloc_aligned(NULL, CACHE_LINE, COPY_BYTES, LICHEN_MEM_BULK);
    if (!a || !b) {
        atomic_fetch_add(&s_w.alloc_fails, 1);
        ESP_LOGE(TAG, "dma worker: no memory for 2 x 1 MB PSRAM");
    }
    for (uint32_t seed = (uint32_t)esp_timer_get_time(); a && b && !stopping(); seed++) {
        dma_round();
        if (!stopping()) {
            copy_round(a, b, seed);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    lichen_free(NULL, a);
    lichen_free(NULL, b);
    worker_end();
}

// One 64 KB cycle under the card's lock. The batch ends with what happened, so a failing card still unmounts as usual.
static void sd_cycle(uint8_t *a, uint8_t *b, uint32_t seed) {
    if (!lichen_sys_sd_begin(SD_WAIT_MS)) {
        nap(200); // the card is busy (the Files app, MeshCore's journal) or just went away
        return;
    }
    mkdir(SD_DIR, 0755); // EEXIST after the first cycle
    fill(a, SD_CHUNK, seed);
    int           err;
    file_result_t r = file_round(SD_FILE, a, b, SD_CHUNK, &err);
    lichen_sys_sd_end(r == FILE_IO ? lichen_sys_sd_io_from_errno(err) : LICHEN_SD_IO_OK);
    atomic_fetch_add(&s_w.sd_kb, SD_CHUNK / 1024);
    count_file(r, err, &s_w.sd_cycles, &s_w.sd_io, "sd");
}

static void sd_worker(void *arg) {
    (void)arg;
    uint8_t *a = lichen_alloc(NULL, SD_CHUNK, LICHEN_MEM_BULK);
    uint8_t *b = lichen_alloc(NULL, SD_CHUNK, LICHEN_MEM_BULK);
    if (!a || !b) {
        atomic_fetch_add(&s_w.alloc_fails, 1);
        ESP_LOGE(TAG, "sd worker: no memory for 2 x 64 KB PSRAM");
    }
    for (uint32_t seed = (uint32_t)esp_timer_get_time(); a && b && !stopping(); seed++) {
        if (atomic_load(&s_w.sd_kb) + SD_CHUNK / 1024 > SD_CAP_KB) {
            nap(1000); // the run's 64 MB are written
            continue;
        }
        if (!lichen_sys_sd_mounted()) {
            ESP_LOGW(TAG, "skip sd: the card went away");
            break;
        }
        sd_cycle(a, b, seed);
        nap(s_sd_pace_ms);
    }
    cleanup_files();
    lichen_free(NULL, a);
    lichen_free(NULL, b);
    worker_end();
}

// A small file on the storage drive every 10 s: flash writes with the cache off, under the load of the others
static void fs_worker(void *arg) {
    (void)arg;
    uint8_t *a = lichen_alloc(NULL, 2 * FS_BYTES, LICHEN_MEM_INTERNAL); // internal, for the run only
    if (!a) {
        atomic_fetch_add(&s_w.alloc_fails, 1);
        ESP_LOGE(TAG, "fs worker: no memory for 1 KB");
    }
    for (uint32_t seed = (uint32_t)esp_timer_get_time(); a && !stopping(); seed++) {
        fill(a, FS_BYTES, seed);
        int           err;
        file_result_t r = file_round(FS_FILE, a, a + FS_BYTES, FS_BYTES, &err);
        count_file(r, err, &s_w.fs_cycles, &s_w.fs_io, "fs");
        nap(FS_PERIOD_MS);
    }
    unlink(FS_FILE);
    lichen_free(NULL, a);
    worker_end();
}

// Wakes every 10 ms and notes how many ms late it woke (a starved core 0 or a long cache-off window)
static void lat_worker(void *arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    while (!stopping()) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(LAT_PERIOD_MS));
        uint32_t late = (uint32_t)((xTaskGetTickCount() - last) * portTICK_PERIOD_MS);
        uint32_t seen = atomic_load(&s_w.lat_worst_ms);
        while (late > seen && !atomic_compare_exchange_weak(&s_w.lat_worst_ms, &seen, late)) {
        }
    }
    worker_end();
}

// ---- the UI load: an LVGL timer, so every redraw runs under the normal lock ----------------------------------------

static void ui_tick(lv_timer_t *t) {
    (void)t;
    lv_obj_invalidate(lv_screen_active()); // a full redraw of whatever is on top
    atomic_fetch_add(&s_w.frames, 1);
}

static void ui_timer_on(void *arg) {
    bool on = arg != NULL;
    if (on && !s_ui_timer) {
        s_ui_timer = lv_timer_create(ui_tick, UI_TICK_MS, NULL);
    } else if (!on && s_ui_timer) {
        lv_timer_delete(s_ui_timer);
        s_ui_timer = NULL;
    }
}

// Off is retried: a timer left behind would keep redrawing after the run
static void ui_timer(bool on) {
    for (int i = 0; i < (on ? 1 : 5); i++) {
        if (lichen_sys_ui_call(ui_timer_on, on ? (void *)1 : NULL, 2000)) {
            return;
        }
    }
    ESP_LOGW(TAG, "ui timer: the LVGL lock was not free, the UI load %s", on ? "does not run" : "still runs");
}

// ---- the monitor ----------------------------------------------------------------------------------------------------

typedef struct {
    stress_run_t run;
    uint32_t     lock_base; // lockcheck hits when the run started
    uint32_t     heap_bad;  // failed integrity checks
    unsigned     samples;
    bool         sd, fs;
    unsigned     minutes;
} mon_t;

static mon_t s_m; // the monitor's only

static void take_sample(stress_sample_t *s) {
    lichen_sys_frame_pool_t tx, rx;
    lichen_sys_mem_frame_pool(true, &tx);
    lichen_sys_mem_frame_pool(false, &rx);
    *s = (stress_sample_t){
        .dma_free    = (uint32_t)lichen_sys_mem_free(LICHEN_SYS_MEM_DMA),
        .dma_min     = (uint32_t)lichen_sys_mem_min(LICHEN_SYS_MEM_DMA),
        .dma_largest = (uint32_t)lichen_sys_mem_largest(LICHEN_SYS_MEM_DMA),
        .int_free    = (uint32_t)lichen_sys_mem_free(LICHEN_SYS_MEM_INTERNAL),
        .int_min     = (uint32_t)lichen_sys_mem_min(LICHEN_SYS_MEM_INTERNAL),
        .int_largest = (uint32_t)lichen_sys_mem_largest(LICHEN_SYS_MEM_INTERNAL),
        .psram_free  = (uint32_t)lichen_sys_mem_free(LICHEN_SYS_MEM_PSRAM),
        .pool_peak   = tx.peak > rx.peak ? tx.peak : rx.peak,
        .pool_drop   = tx.drops + rx.drops,
        .lat_ms      = atomic_exchange(&s_w.lat_worst_ms, 0),
    };
}

static uint32_t start_free(char const *name) {
    for (size_t i = 0; i < s_stacks.n_start && i < STACKS_MAX; i++) {
        if (strcmp(s_stacks.start[i].name, name) == 0) {
            return s_stacks.start[i].free;
        }
    }
    return UINT32_MAX; // a task that started during the run: all its use is the run's
}

// The lowest stack free of the tasks whose high-water fell under the floor during the run (UINT32_MAX: none); the
// first such task is logged once
static uint32_t stack_low(void) {
    s_stacks.n_now = lichen_sys_mem_stacks(s_stacks.now, STACKS_MAX);
    uint32_t low   = UINT32_MAX;
    for (size_t i = 0; i < s_stacks.n_now && i < STACKS_MAX; i++) {
        lichen_sys_stack_t const *t = &s_stacks.now[i];
        if (stress_stack_fell_low(t->free, start_free(t->name)) && t->free < low) {
            low = t->free;
            ESP_LOGE(TAG, "stack low %s %u/%u", t->name, (unsigned)t->free, (unsigned)t->size);
        }
    }
    return low;
}

static void counts(stress_counts_t *c) {
    *c = (stress_counts_t){
        .dma_rounds = atomic_load(&s_w.dma_rounds), .dma_busy = atomic_load(&s_w.dma_busy),
        .dma_bad = atomic_load(&s_w.dma_bad), .copies = atomic_load(&s_w.copies),
        .mismatches = atomic_load(&s_w.mismatches), .alloc_fails = atomic_load(&s_w.alloc_fails),
        .heap_bad = s_m.heap_bad, .lock_hits = lichen_sys_ui_lockcheck_hits() - s_m.lock_base,
        .sd_cycles = atomic_load(&s_w.sd_cycles), .sd_kb = atomic_load(&s_w.sd_kb), .sd_io = atomic_load(&s_w.sd_io),
        .fs_cycles = atomic_load(&s_w.fs_cycles), .fs_io = atomic_load(&s_w.fs_io), .frames = atomic_load(&s_w.frames),
    };
}

static void publish(uint32_t t_s, stress_sample_t const *last, bool running) {
    stress_counts_t c;
    counts(&c);
    portENTER_CRITICAL(&s_view_mux);
    s_view.running = running;
    s_view.t_s     = t_s;
    s_view.counts  = c;
    s_view.run     = s_m.run;
    if (last) {
        s_view.last = *last;
    }
    portEXIT_CRITICAL(&s_view_mux);
}

static bool heap_sound(bool with_psram) {
    bool ok = lichen_sys_mem_check(LICHEN_SYS_MEM_INTERNAL) && (!with_psram || lichen_sys_mem_check(LICHEN_SYS_MEM_PSRAM));
    s_m.heap_bad += !ok;
    return ok;
}

static void log_stop(stress_reason_t r, uint32_t t_s) {
    char line[STRESS_LINE_MAX];
    stress_fmt_stop(line, sizeof(line), r, t_s);
    if (stress_failed(r)) {
        ESP_LOGE(TAG, "%s", line);
    } else {
        ESP_LOGW(TAG, "%s", line); // Esc, stop, power off, an update: the run ended early, nothing failed
    }
}

// One 10 s check: the figures, the rules, the line
static void sample_and_judge(uint32_t t_s, stress_reason_t outside) {
    stress_obs_t o;
    take_sample(&o.s);
    stress_counts_t c;
    counts(&c);
    o.mismatches  = c.mismatches;
    o.dma_bad     = c.dma_bad;
    o.alloc_fails = c.alloc_fails;
    o.lock_hits   = c.lock_hits;
    o.heap_ok     = heap_sound(++s_m.samples % PSRAM_CHECK_EVERY == 0);
    o.stack_low   = stack_low();
    stress_track(&s_m.run, &o.s);
    stress_reason_t r = stress_judge(&o);
    if (stress_latch(&s_m.run, r != STRESS_NONE ? r : outside, t_s)) {
        log_stop(s_m.run.reason, t_s);
    }
    char line[STRESS_LINE_MAX];
    stress_fmt_sample(line, sizeof(line), t_s, &o.s, s_m.run.reason);
    ESP_LOGI(TAG, "%s", line);
    publish(t_s, &o.s, true);
}

// A stop from outside the rules: asked for, power off, an update
static stress_reason_t outside_stop(void) {
    stress_reason_t r = (stress_reason_t)atomic_load(&s_stop_req);
    if (r != STRESS_NONE) {
        return r;
    }
    if (power_going_down()) {
        return STRESS_POWEROFF;
    }
    return update_screen_busy() ? STRESS_UPDATE : STRESS_NONE;
}

static void start_worker(char const *name, void (*fn)(void *), uint32_t stack, lichen_prio_t prio) {
    atomic_fetch_add(&s_w.workers, 1);
    if (!lichen_task_start_in(NULL, name, fn, NULL, stack, prio, LICHEN_STACK_INTERNAL, NULL)) {
        atomic_fetch_sub(&s_w.workers, 1);
        atomic_fetch_add(&s_w.alloc_fails, 1);
        ESP_LOGE(TAG, "%s: no task slot or no memory for its stack", name);
    }
}

// Before the workers: leftovers of an earlier run go, the baseline stacks and lockcheck count are taken, the start line
static void run_begin(void) {
    cleanup_files();
    s_stacks.n_start = lichen_sys_mem_stacks(s_stacks.start, STACKS_MAX);
    s_m.lock_base    = lichen_sys_ui_lockcheck_hits();
    s_m.sd           = lichen_sys_sd_mounted();
    s_m.fs           = lichen_sys_storage_mounted();
    s_sd_pace_ms     = (uint32_t)((uint64_t)s_m.minutes * 60000u / (SD_CAP_KB / (SD_CHUNK / 1024)));
    char line[STRESS_LINE_MAX];
    stress_fmt_start(line, sizeof(line), s_m.minutes, s_m.sd, s_m.fs);
    ESP_LOGI(TAG, "%s", line);
    if (!s_m.sd) {
        ESP_LOGW(TAG, "skip sd: no card");
    }
    if (!s_m.fs) {
        ESP_LOGW(TAG, "skip fs: the storage drive is not mounted");
    }
    portENTER_CRITICAL(&s_view_mux);
    s_view.sd = s_m.sd;
    s_view.fs = s_m.fs;
    portEXIT_CRITICAL(&s_view_mux);
    start_worker("stress-dma", dma_worker, 6144, LICHEN_PRIO_BACKGROUND);
    if (s_m.sd) {
        start_worker("stress-sd", sd_worker, 6144, LICHEN_PRIO_BACKGROUND);
    }
    if (s_m.fs) {
        start_worker("stress-fs", fs_worker, 4096, LICHEN_PRIO_BACKGROUND);
    }
    start_worker("stress-lat", lat_worker, 2048, LICHEN_PRIO_SERVICE);
    ui_timer(true);
}

// The workers end their loops within a round (a DMA gate round takes a few seconds); the files go after them
static void run_end(void) {
    atomic_store(&s_w.stop, true);
    ui_timer(false);
    int64_t t0 = esp_timer_get_time();
    while (atomic_load(&s_w.workers) > 0) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (esp_timer_get_time() - t0 > 30 * 1000000LL) {
            ESP_LOGE(TAG, "%d worker(s) still running 30 s after the stop", atomic_load(&s_w.workers));
            t0 = esp_timer_get_time();
        }
    }
    cleanup_files();
}

static void log_report(uint32_t t_s) {
    char line[STRESS_LINE_MAX];
    bool heap_ok = heap_sound(true); // every heap once more, PSRAM included: a break after the last sample counts
    if (!heap_ok && stress_latch(&s_m.run, STRESS_HEAP, t_s)) {
        log_stop(STRESS_HEAP, t_s);
    }
    bool pass = heap_ok && !stress_failed(s_m.run.reason);
    stress_fmt_done(line, sizeof(line), t_s, pass);
    if (pass) {
        ESP_LOGI(TAG, "%s", line);
    } else {
        ESP_LOGE(TAG, "%s", line);
    }
    if (s_m.run.have) {
        stress_fmt_min(line, sizeof(line), &s_m.run.lo);
        ESP_LOGI(TAG, "%s", line);
    }
    s_stacks.n_now = lichen_sys_mem_stacks(s_stacks.now, STACKS_MAX);
    for (size_t i = 0; i < s_stacks.n_now && i < STACKS_MAX; i++) {
        stress_fmt_stack(line, sizeof(line), s_stacks.now[i].name, s_stacks.now[i].free, s_stacks.now[i].size);
        ESP_LOGI(TAG, "%s", line);
    }
    stress_counts_t c;
    counts(&c);
    stress_fmt_errors(line, sizeof(line), &c);
    ESP_LOGI(TAG, "%s", line);
    stress_fmt_work(line, sizeof(line), &c);
    ESP_LOGI(TAG, "%s", line);
}

static void mon_task(void *arg) {
    (void)arg;
    run_begin();
    int64_t  t0    = esp_timer_get_time();
    uint32_t limit = s_m.minutes * 60u, next = SAMPLE_S, t_s = 0;
    while (s_m.run.reason == STRESS_NONE && t_s < limit) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        t_s                     = (uint32_t)((esp_timer_get_time() - t0) / 1000000);
        stress_reason_t outside = outside_stop();
        if (t_s >= next || t_s >= limit || outside != STRESS_NONE) {
            sample_and_judge(t_s, outside);
            next = t_s + SAMPLE_S;
        } else {
            publish(t_s, NULL, true);
        }
    }
    run_end();
    log_report(t_s);
    portENTER_CRITICAL(&s_view_mux);
    s_view.ended = true;
    portEXIT_CRITICAL(&s_view_mux);
    publish(t_s, NULL, false);
    atomic_store(&s_on, false);
}

// ---- the calls ------------------------------------------------------------------------------------------------------

static bool c6_trouble(void) {
    char state[48];
    return shell_sources.c6_trouble && shell_sources.c6_trouble(state, sizeof(state));
}

char const *stress_start(unsigned minutes) {
    if (minutes < 1 || minutes > STRESS_MIN_MAX) {
        return "1 to 30 minutes";
    }
    thermal_info_t th;
    thermal_info(&th);
    stress_gate_in_t in = {.running     = atomic_load(&s_on),
                           .update      = update_screen_busy(),
                           .poweroff    = power_going_down(),
                           .c6_trouble  = c6_trouble(),
                           .hot         = th.level != TH_OK,
                           .psram_trial = psram_speed_trial_pending(),
                           .dma_free    = (uint32_t)lichen_sys_mem_free(LICHEN_SYS_MEM_DMA),
                           .psram_free  = (uint32_t)lichen_sys_mem_free(LICHEN_SYS_MEM_PSRAM)};
    char const *why = stress_refusal(&in);
    if (why || atomic_exchange(&s_on, true)) {
        return why ? why : "a stress run is already on";
    }
    memset(&s_w, 0, sizeof(s_w));
    memset(&s_m, 0, sizeof(s_m));
    s_m.minutes = minutes;
    atomic_store(&s_stop_req, STRESS_NONE);
    portENTER_CRITICAL(&s_view_mux);
    s_view = (stress_view_t){.running = true, .minutes = minutes};
    portEXIT_CRITICAL(&s_view_mux);
    if (!lichen_task_start_in(NULL, "stress-mon", mon_task, NULL, 4096, LICHEN_PRIO_SERVICE, LICHEN_STACK_INTERNAL,
                              &s_mon)) {
        portENTER_CRITICAL(&s_view_mux);
        s_view.running = false;
        portEXIT_CRITICAL(&s_view_mux);
        atomic_store(&s_on, false);
        return "no task slot or no memory for the monitor";
    }
    return NULL;
}

void stress_stop(stress_reason_t why) {
    int none = STRESS_NONE;
    if (atomic_load(&s_on) && atomic_compare_exchange_strong(&s_stop_req, &none, (int)why) && s_mon) {
        xTaskNotifyGive(s_mon);
    }
}

void stress_view(stress_view_t *out) {
    portENTER_CRITICAL(&s_view_mux);
    *out = s_view;
    portEXIT_CRITICAL(&s_view_mux);
}

bool stress_running(void) {
    return atomic_load(&s_on);
}
