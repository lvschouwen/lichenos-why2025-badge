// The stress run's load (#278): five workers on the OS's tasks, one LVGL timer and a monitor that samples every 10 s,
// judges the run (stress_calc.c) and writes the report. Workers: stress-dma (DMA gate rounds through psram_guard plus a
// data-verified 1 MB PSRAM copy), stress-sd (64 KB write, read back, compare, delete on the card, paced so a run writes
// at most 64 MB), stress-fs (a 512 B file on /storage every 10 s), stress-lat (a 10 ms probe: how late it wakes) and the
// UI timer (a full redraw of the screen on top every 100 ms). LoRa is never touched: nothing is sent. Every line goes
// out through ESP_LOG (tag "stress"), so the UART and the log store see the same lines however the run was started.
//
// Lifetimes: workers and stress_stop never use the monitor's task handle (it is gone once a run ended). They give the
// two semaphores below, which live for as long as the badge runs. The monitor clears s_on only once every worker has
// returned, so stress_start's reset of s_w never meets a worker of the run before.
#include "stress_workers.h"

#include "esp_cache.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "files_model.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lichen_mem.h"
#include "lichen_sys_mem.h"
#include "lichen_sys_storage.h"
#include "lichen_sys_ui.h"
#include "ui_remote.h"
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
#define STACKS_MAX    64
#define HUNG_S        60 // workers still running this long after the stop: the run fails
#define FLUSH         (ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE)

typedef struct {
    _Atomic uint32_t c[STRESS_C_COUNT];
    _Atomic uint32_t lat_worst_ms; // since the last sample
    _Atomic uint32_t round_seq;    // odd while a DMA gate round runs
    atomic_uint      fail_mask;    // STRESS_W_*: the workers that saw their own data go wrong
    atomic_bool      stop;         // every worker ends its loop
    atomic_int       workers;      // still running
} shared_t;

static shared_t          s_w;          // internal: workers on internal stacks read it with the cache off (flash writes)
static atomic_bool       s_on;         // a run is on: set by stress_start, cleared by the monitor after its workers
static atomic_int        s_stop_req;   // the first stress_stop reason, STRESS_NONE when none
static StaticSemaphore_t s_wake_buf, s_done_buf;
static SemaphoreHandle_t s_wake;       // stress_stop -> the monitor's 1 s wait
static SemaphoreHandle_t s_done;       // a worker returned -> the monitor's wait at the end
static portMUX_TYPE      s_view_mux = portMUX_INITIALIZER_UNLOCKED;
static stress_view_t     s_view;       // under s_view_mux, internal (a critical section never touches PSRAM)
static uint32_t          s_sd_pace_ms; // the SD worker's gap between cycles for this run

typedef struct {
    lichen_sys_stack_t now[STACKS_MAX], start[STACKS_MAX];
    size_t             n_now, n_start;
} stacks_t;
static stacks_t *s_st; // the monitor's only, in PSRAM for the run (NULL: no stack figures this run)

static void add(stress_count_t i, uint32_t n) {
    atomic_fetch_add(&s_w.c[i], n);
}

// ---- patterns and files ---------------------------------------------------------------------------------------------

// xorshift32 from seed, one word per step: every word differs, a stuck bit shows. fill writes it, check compares.
static bool pattern(uint8_t *buf, size_t n, uint32_t seed, bool write) {
    uint32_t *w = (uint32_t *)buf, x = seed | 1u;
    for (size_t i = 0; i < n / 4; i++) {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        if (write) {
            w[i] = x;
        } else if (w[i] != x) {
            return false;
        }
    }
    return true;
}

static int err_now(void) {
    return errno ? errno : EIO;
}

// Writes a, reads it back into b, deletes the file. 0, the errno of the failed call, or -1 for different data.
static int file_round(char const *path, uint8_t const *a, uint8_t *b, size_t n) {
    errno  = 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int err = fd < 0 ? err_now() : write(fd, a, n) == (ssize_t)n ? 0 : err_now();
    if (fd >= 0 && close(fd) != 0 && !err) {
        err = err_now();
    }
    if (!err) {
        memset(b, 0, n);
        fd  = open(path, O_RDONLY);
        err = fd < 0 ? err_now() : read(fd, b, n) == (ssize_t)n ? 0 : err_now();
        if (fd >= 0) {
            close(fd);
        }
    }
    unlink(path);
    return err ? err : memcmp(a, b, n) == 0 ? 0 : -1;
}

static void count_file(int r, stress_count_t done, stress_count_t io, unsigned worker, char const *what) {
    if (r != 0) {
        atomic_fetch_or(&s_w.fail_mask, worker);
    }
    if (r == 0) {
        add(done, 1);
    } else if (r < 0) {
        add(STRESS_C_MISMATCH, 1);
        ESP_LOGE(TAG, "%s: read back different", what);
    } else {
        add(io, 1);
        ESP_LOGW(TAG, "%s: file error %d", what, r);
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
    xSemaphoreGive(s_done); // lives for good: safe however late this worker is
}

// Two PSRAM buffers for a worker; false (counted, logged) without them
static bool buffers(uint8_t **a, uint8_t **b, size_t n, char const *what) {
    *a = lichen_alloc_aligned(NULL, CACHE_LINE, n, LICHEN_MEM_BULK);
    *b = lichen_alloc_aligned(NULL, CACHE_LINE, n, LICHEN_MEM_BULK);
    if (*a && *b) {
        return true;
    }
    add(STRESS_C_ALLOC, 1);
    ESP_LOGE(TAG, "%s: no memory", what);
    return false;
}

static void dma_round(void) {
    psram_dma_round_info_t info;
    atomic_fetch_add(&s_w.round_seq, 1);
    psram_dma_round_t r = psram_dma_quick_round(&info);
    atomic_fetch_add(&s_w.round_seq, 1);
    uint32_t bad = info.bad_bytes + info.errors;
    if (r == PSRAM_DMA_ROUND_BUSY) { // never a pass: another gate holds the engine (#278)
        add(STRESS_C_DMA_BUSY, 1);
        ESP_LOGW(TAG, "dma: skipped, gate busy");
        nap(1000);
    } else if (r == PSRAM_DMA_ROUND_NOT_RUN) {
        add(STRESS_C_ALLOC, 1);
        ESP_LOGE(TAG, "dma: no memory");
    } else {
        add(STRESS_C_DMA, 1);
        add(STRESS_C_DMA_BAD, r == PSRAM_DMA_ROUND_FAIL ? (bad ? bad : 1) : 0);
        if (r == PSRAM_DMA_ROUND_FAIL) {
            atomic_fetch_or(&s_w.fail_mask, STRESS_W_DMA);
            ESP_LOGE(TAG, "dma: %u bad bytes, %u errors", (unsigned)info.bad_bytes, info.errors);
        }
    }
}

// 1 MB PSRAM to PSRAM by the CPU, both buffers pushed out of the cache, so the check reads what reached the RAM
static void copy_round(uint8_t *a, uint8_t *b, uint32_t seed) {
    pattern(a, COPY_BYTES, seed, true);
    esp_cache_msync(a, COPY_BYTES, FLUSH);
    memcpy(b, a, COPY_BYTES);
    esp_cache_msync(b, COPY_BYTES, FLUSH);
    count_file(pattern(b, COPY_BYTES, seed, false) ? 0 : -1, STRESS_C_COPIES, STRESS_C_COPIES, STRESS_W_DMA, "psram copy");
}

static void dma_worker(void *arg) {
    (void)arg;
    uint8_t *a, *b;
    bool     ok = buffers(&a, &b, COPY_BYTES, "dma");
    for (uint32_t seed = (uint32_t)esp_timer_get_time(); ok && !stopping(); seed++) {
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
    pattern(a, SD_CHUNK, seed, true);
    int r = file_round(SD_FILE, a, b, SD_CHUNK);
    lichen_sys_sd_end(r > 0 ? lichen_sys_sd_io_from_errno(r) : LICHEN_SD_IO_OK);
    add(STRESS_C_SD_KB, SD_CHUNK / 1024);
    count_file(r, STRESS_C_SD, STRESS_C_SD_IO, STRESS_W_SD, "sd");
}

static void sd_worker(void *arg) {
    (void)arg;
    uint8_t *a, *b;
    bool     ok = buffers(&a, &b, SD_CHUNK, "sd");
    for (uint32_t seed = (uint32_t)esp_timer_get_time(); ok && !stopping(); seed++) {
        if (atomic_load(&s_w.c[STRESS_C_SD_KB]) + SD_CHUNK / 1024 > SD_CAP_KB) {
            nap(1000); // the run's 64 MB are written
        } else if (!lichen_sys_sd_mounted()) {
            ESP_LOGW(TAG, "sd: skipped, card gone");
            break;
        } else {
            sd_cycle(a, b, seed);
            nap(s_sd_pace_ms);
        }
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
        add(STRESS_C_ALLOC, 1);
        ESP_LOGE(TAG, "fs: no memory");
    }
    for (uint32_t seed = (uint32_t)esp_timer_get_time(); a && !stopping(); seed++) {
        pattern(a, FS_BYTES, seed, true);
        count_file(file_round(FS_FILE, a, a + FS_BYTES, FS_BYTES), STRESS_C_FS, STRESS_C_FS_IO, STRESS_W_FS, "fs");
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

void stress_ui_frame(void) {
    add(STRESS_C_FRAMES, 1);
}

// ---- the monitor ----------------------------------------------------------------------------------------------------

typedef struct {
    stress_run_t run;
    uint32_t     lock_base; // lockcheck hits when the run started
    unsigned     samples;
    bool         sd, fs;
    unsigned     minutes;
} mon_t;

static mon_t s_m; // the monitor's only

static void take_sample(stress_sample_t *s) {
    lichen_sys_frame_pool_t tx, rx;
    lichen_sys_mem_frame_pool(true, &tx);
    lichen_sys_mem_frame_pool(false, &rx);
    lichen_sys_mem_kind_t const kind[] = {LICHEN_SYS_MEM_DMA, LICHEN_SYS_MEM_INTERNAL};
    uint32_t                    v[6];
    for (int i = 0; i < 2; i++) {
        v[i * 3]     = (uint32_t)lichen_sys_mem_free(kind[i]);
        v[i * 3 + 1] = (uint32_t)lichen_sys_mem_min(kind[i]);
        v[i * 3 + 2] = (uint32_t)lichen_sys_mem_largest(kind[i]);
    }
    *s = (stress_sample_t){v[0], v[1], v[2], v[3], v[4], v[5], (uint32_t)lichen_sys_mem_free(LICHEN_SYS_MEM_PSRAM),
                           tx.peak > rx.peak ? tx.peak : rx.peak, tx.drops + rx.drops,
                           atomic_exchange(&s_w.lat_worst_ms, 0)};
}

static uint32_t start_free(char const *name) {
    for (size_t i = 0; i < s_st->n_start && i < STACKS_MAX; i++) {
        if (strcmp(s_st->start[i].name, name) == 0) {
            return s_st->start[i].free;
        }
    }
    return UINT32_MAX; // a task that started during the run: all its use is the run's
}

// The lowest stack free of the tasks that fell low during the run (UINT32_MAX: none), each such task logged
static uint32_t stack_low(void) {
    if (!s_st) {
        return UINT32_MAX;
    }
    s_st->n_now = lichen_sys_mem_stacks(s_st->now, STACKS_MAX);
    uint32_t low   = UINT32_MAX;
    for (size_t i = 0; i < s_st->n_now && i < STACKS_MAX; i++) {
        lichen_sys_stack_t const *t = &s_st->now[i];
        if (stress_stack_fell_low(t->free, start_free(t->name)) && t->free < low) {
            low = t->free;
            ESP_LOGE(TAG, "stack low %s %u/%u", t->name, (unsigned)t->free, (unsigned)t->size);
        }
    }
    return low;
}

static void counts(stress_counts_t *c) {
    for (int i = 0; i < STRESS_C_COUNT; i++) {
        c->n[i] = atomic_load(&s_w.c[i]);
    }
    c->n[STRESS_C_LOCK] = stress_since(lichen_sys_ui_lockcheck_hits(), s_m.lock_base);
}

static void publish(uint32_t t_s, stress_sample_t const *last, bool running) {
    stress_counts_t c;
    counts(&c);
    portENTER_CRITICAL(&s_view_mux);
    s_view.running = running;
    s_view.ended   = !running;
    s_view.t_s     = t_s;
    s_view.counts  = c;
    s_view.fail    = atomic_load(&s_w.fail_mask);
    s_view.run     = s_m.run;
    if (last) {
        s_view.last = *last;
    }
    portEXIT_CRITICAL(&s_view_mux);
}

static bool heap_sound(bool with_psram) {
    bool ok = lichen_sys_mem_check(LICHEN_SYS_MEM_INTERNAL) && (!with_psram || lichen_sys_mem_check(LICHEN_SYS_MEM_PSRAM));
    add(STRESS_C_HEAP, !ok);
    return ok;
}

// A failure at E; Esc, stop, power off, an update, the trial, heat or the C6 at W (the run ended early, nothing failed)
static void latch(stress_reason_t r, uint32_t t_s) {
    if (stress_latch(&s_m.run, r, t_s)) {
        char line[32];
        stress_fmt_stop(line, sizeof(line), r, t_s);
        ESP_LOG_LEVEL(stress_failed(r) ? ESP_LOG_ERROR : ESP_LOG_WARN, TAG, "%s", line);
    }
}

// One 10 s check: the figures, the rules, the line. The DMA floor counts only when no gate round overlapped the sample.
static void sample_and_judge(uint32_t t_s, stress_reason_t outside) {
    stress_obs_t o;
    uint32_t     seq = atomic_load(&s_w.round_seq);
    take_sample(&o.s);
    o.in_dma_round = (seq & 1u) || seq != atomic_load(&s_w.round_seq);
    stress_counts_t c;
    counts(&c);
    o.mismatches  = c.n[STRESS_C_MISMATCH];
    o.dma_bad     = c.n[STRESS_C_DMA_BAD];
    o.alloc_fails = c.n[STRESS_C_ALLOC];
    o.lock_hits   = c.n[STRESS_C_LOCK];
    o.heap_ok     = heap_sound(++s_m.samples % PSRAM_CHECK_EVERY == 0);
    o.stack_low   = stack_low();
    stress_track(&s_m.run, &o.s);
    stress_reason_t r = stress_judge(&o);
    latch(r != STRESS_NONE ? r : outside, t_s);
    char line[STRESS_LINE_MAX];
    stress_fmt_sample(line, sizeof(line), t_s, &o.s, s_m.run.reason);
    ESP_LOGI(TAG, "%s", line);
    publish(t_s, &o.s, true);
}

static bool c6_trouble(void) {
    char state[48];
    return shell_sources.c6_trouble && shell_sources.c6_trouble(state, sizeof(state));
}

static bool hot(void) {
    thermal_info_t th;
    thermal_info(&th);
    return th.level != TH_OK;
}

// A stop from outside the rules, every second
static stress_reason_t outside_stop(void) {
    if (!ui_remote_locked() && atomic_load(&s_stop_req) == STRESS_NONE) {
        stress_stop(STRESS_ESC); // the key lock ended: Esc held 3 s (or fw ui lock off)
    }
    ui_remote_touch(); // the lock's timeout counts from the last command: the run is one
    stress_outside_t in = {.asked      = (stress_reason_t)atomic_load(&s_stop_req),
                           .poweroff   = power_going_down(),
                           .update     = update_screen_busy(),
                           .trial      = psram_speed_trial_pending(),
                           .hot        = hot(),
                           .c6_trouble = c6_trouble()};
    return stress_outside(&in);
}

static void start_worker(char const *name, void (*fn)(void *), uint32_t stack, lichen_prio_t prio) {
    atomic_fetch_add(&s_w.workers, 1);
    if (!lichen_task_start_in(NULL, name, fn, NULL, stack, prio, LICHEN_STACK_INTERNAL, NULL)) {
        atomic_fetch_sub(&s_w.workers, 1);
        add(STRESS_C_ALLOC, 1);
        ESP_LOGE(TAG, "%s: no memory", name);
    }
}

// Before the workers: leftovers of an earlier run go, the baseline stacks and lockcheck count are taken, the start line
static void run_begin(void) {
    cleanup_files();
    s_st = lichen_alloc(NULL, sizeof(*s_st), LICHEN_MEM_BULK);
    if (!s_st) {
        ESP_LOGW(TAG, "stack: skipped, no memory");
    } else {
        s_st->n_start = lichen_sys_mem_stacks(s_st->start, STACKS_MAX);
    }
    s_m.lock_base    = lichen_sys_ui_lockcheck_hits();
    s_m.sd           = lichen_sys_sd_mounted();
    s_m.fs           = lichen_sys_storage_mounted();
    s_sd_pace_ms     = (uint32_t)((uint64_t)s_m.minutes * 60000u / (SD_CAP_KB / (SD_CHUNK / 1024)));
    char line[80];
    stress_fmt_start(line, sizeof(line), s_m.minutes, s_m.sd, s_m.fs);
    ESP_LOGI(TAG, "%s", line);
    char const *skip[] = {s_m.sd ? NULL : "sd: skipped, no card", s_m.fs ? NULL : "fs: skipped, no drive"};
    for (int i = 0; i < 2; i++) {
        if (skip[i]) {
            ESP_LOGW(TAG, "%s", skip[i]);
        }
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
    ui_remote_hold(true); // the physical keys, for the whole run: Esc held 3 s takes them back and stops it (#190)
    stress_screen_open(); // also the UI load: it redraws itself every 100 ms while the run is on
}

// Waits for the workers to return; false once HUNG_S passed with some still running (the run fails then)
static bool workers_ended(uint32_t wait_s) {
    for (uint32_t s = 0; atomic_load(&s_w.workers) > 0; s++) {
        if (s >= wait_s) {
            ESP_LOGE(TAG, "%d worker(s) hung", atomic_load(&s_w.workers));
            return false;
        }
        xSemaphoreTake(s_done, pdMS_TO_TICKS(1000));
    }
    return true;
}

static void log_report(uint32_t t_s, bool hung) {
    char line[STRESS_LINE_MAX];
    bool heap_ok = heap_sound(true); // every heap once more, PSRAM included: a break after the last sample counts
    latch(hung ? STRESS_HUNG : heap_ok ? STRESS_NONE : STRESS_HEAP, t_s);
    bool pass = !hung && heap_ok && !stress_failed(s_m.run.reason);
    stress_fmt_done(line, sizeof(line), t_s, pass);
    ESP_LOG_LEVEL(pass ? ESP_LOG_INFO : ESP_LOG_ERROR, TAG, "%s", line);
    if (s_m.run.have) {
        stress_fmt_min(line, sizeof(line), &s_m.run.lo);
        ESP_LOGI(TAG, "%s", line);
    }
    size_t n = s_st ? lichen_sys_mem_stacks(s_st->now, STACKS_MAX) : 0;
    for (size_t i = 0; i < n && i < STACKS_MAX; i++) {
        stress_fmt_stack(line, sizeof(line), s_st->now[i].name, s_st->now[i].free, s_st->now[i].size);
        ESP_LOGI(TAG, "%s", line);
    }
    stress_counts_t c;
    counts(&c);
    for (int work = 0; work < 2; work++) {
        stress_fmt_counts(line, sizeof(line), &c, work);
        ESP_LOGI(TAG, "%s", line);
    }
}

static void mon_task(void *arg) {
    (void)arg;
    run_begin();
    int64_t  t0    = esp_timer_get_time();
    uint32_t limit = s_m.minutes * 60u, next = SAMPLE_S, t_s = 0;
    while (s_m.run.reason == STRESS_NONE && t_s < limit) {
        xSemaphoreTake(s_wake, pdMS_TO_TICKS(1000));
        t_s                     = (uint32_t)((esp_timer_get_time() - t0) / 1000000);
        stress_reason_t outside = outside_stop();
        if (t_s >= next || t_s >= limit || outside != STRESS_NONE) {
            sample_and_judge(t_s, outside);
            next = t_s + SAMPLE_S;
        } else {
            publish(t_s, NULL, true);
        }
    }
    atomic_store(&s_w.stop, true);
    ui_remote_hold(false); // the keys are free again for the result screen (a key press closes it)
    bool hung = !workers_ended(HUNG_S);
    cleanup_files();
    log_report(t_s, hung);
    workers_ended(UINT32_MAX); // a hung worker still owns s_w: no new run before it returns
    lichen_free(NULL, s_st);
    s_st = NULL;
    publish(t_s, NULL, false);
    atomic_store(&s_on, false);
}

// ---- the calls ------------------------------------------------------------------------------------------------------

char const *stress_start(unsigned minutes) {
    stress_gate_in_t in = {.running     = atomic_load(&s_on),
                           .update      = update_screen_busy(),
                           .poweroff    = power_going_down(),
                           .c6_trouble  = c6_trouble(),
                           .hot         = hot(),
                           .psram_trial = psram_speed_trial_pending(),
                           .dma_free    = (uint32_t)lichen_sys_mem_free(LICHEN_SYS_MEM_DMA),
                           .psram_free  = (uint32_t)lichen_sys_mem_free(LICHEN_SYS_MEM_PSRAM)};
    char const *why = minutes < 1 || minutes > STRESS_MIN_MAX ? "1 to 30 minutes" : stress_refusal(&in);
    if (why || atomic_exchange(&s_on, true)) {
        return why ? why : "a stress run is already on";
    }
    if (!s_wake) { // once, for good (static memory: nothing to fail); s_on makes this single-threaded
        s_wake = xSemaphoreCreateBinaryStatic(&s_wake_buf);
        s_done = xSemaphoreCreateBinaryStatic(&s_done_buf);
    }
    xSemaphoreTake(s_wake, 0); // a stop given after the last run ended
    memset(&s_w, 0, sizeof(s_w)); // no worker is left: the last monitor cleared s_on only after all of them
    memset(&s_m, 0, sizeof(s_m));
    s_m.minutes = minutes;
    atomic_store(&s_stop_req, STRESS_NONE);
    portENTER_CRITICAL(&s_view_mux);
    s_view = (stress_view_t){.running = true, .minutes = minutes};
    portEXIT_CRITICAL(&s_view_mux);
    if (!lichen_task_start_in(NULL, "stress-mon", mon_task, NULL, 4096, LICHEN_PRIO_SERVICE, LICHEN_STACK_INTERNAL,
                              NULL)) {
        portENTER_CRITICAL(&s_view_mux);
        s_view.running = false;
        portEXIT_CRITICAL(&s_view_mux);
        atomic_store(&s_on, false);
        return "no memory for the monitor";
    }
    return NULL;
}

void stress_stop(stress_reason_t why) {
    int none = STRESS_NONE;
    if (atomic_load(&s_on) && atomic_compare_exchange_strong(&s_stop_req, &none, (int)why)) {
        xSemaphoreGive(s_wake);
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
