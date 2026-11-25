// "fw vbench [n] [flip]" (#558 spike, developer builds only): n scale + rotate PPA frames of the planned DOOM video path
// (#527), the time per frame, and the PPA's scaling filter read from the pixels. Each frame is one job on the LVGL task
// through the UI service (display_vbench_frame needs the PPA client's core and the lock), the console task only waits for
// it, so the LVGL lock is never held across frames. The numbers go on #527; nothing here ships in a release image.
#include "vbench_cmd.h"

#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH && CONFIG_LICHEN_DEV_COMMANDS

#include "display_info.h"
#include "display_vbench.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "lichen_sys_ui.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VBENCH_DEFAULT 100
#define VBENCH_MAX     600
#define SRC_BYTES      (VBENCH_SRC_W * VBENCH_SRC_H * 2)
#define I8_BYTES       (VBENCH_SRC_W * VBENCH_SRC_H)
#define DISTINCT_MAX   8
#define JOB_WAIT_MS    3000
#define INSET          4 // pixels left out of the scan at every edge of the window: a column the PPA did not write is not a filter

typedef struct {
    uint16_t v[DISTINCT_MAX];
    unsigned n;
    bool     more;
} distinct_t;

// One job at a time: static, so a job that outlives a timed-out wait writes into memory that is still there
static struct {
    SemaphoreHandle_t done;
    void const       *src;
    uint8_t const    *idx; // flip mode: the engine's 8-bit frame, expanded into src through lut inside the job
    uint16_t          lut[256];
    bool              scan, redraw, flip;
    vbench_status_t   status;
    vbench_frame_t    frame;
    uint32_t          expand_us, job_us;
    distinct_t        seen;
    volatile bool     running;
} job;

// Four quadrants split at the middle of the source, so one vertical and one horizontal edge cross the picture: a nearest
// filter leaves two values in the output, a bilinear one greys along both edges. Built in place (no stack) as 565 words,
// or in flip mode as palette indexes 0 and 1 with lut[1] white. Written whole before every frame, as the real path writes
// a new picture, so the driver's source write-back is in the time.
static void fill_pattern(uint16_t *src) {
    uint16_t *const top = src, *const bottom = src + (VBENCH_SRC_H / 2) * VBENCH_SRC_W;
    for (int x = 0; x < VBENCH_SRC_W; x++) {
        top[x]    = x < VBENCH_SRC_W / 2 ? 0x0000 : 0xFFFF;
        bottom[x] = (uint16_t)~top[x];
    }
    for (int y = 0; y < VBENCH_SRC_H; y++) {
        uint16_t *row = src + y * VBENCH_SRC_W;
        if (row != top && row != bottom) {
            memcpy(row, y < VBENCH_SRC_H / 2 ? top : bottom, VBENCH_SRC_W * sizeof(uint16_t));
        }
    }
}

static void fill_indexes(uint8_t *idx) {
    for (int y = 0; y < VBENCH_SRC_H; y++) {
        for (int x = 0; x < VBENCH_SRC_W; x++) {
            idx[y * VBENCH_SRC_W + x] = (x < VBENCH_SRC_W / 2) != (y < VBENCH_SRC_H / 2);
        }
    }
    for (int i = 0; i < 256; i++) {
        job.lut[i] = i == 1 ? 0xFFFF : 0x0000;
    }
}

static void note_value(distinct_t *d, uint16_t px) {
    for (unsigned i = 0; i < d->n; i++) {
        if (d->v[i] == px) {
            return;
        }
    }
    if (d->n < DISTINCT_MAX) {
        d->v[d->n++] = px;
    } else {
        d->more = true;
    }
}

// The distinct pixel values inside the output window, read after an invalidate so no stale cache line answers for the PPA
static void scan_window(vbench_frame_t const *f, distinct_t *d) {
    esp_cache_msync((void *)f->fb, 720 * 720 * sizeof(uint16_t), ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    for (int y = INSET; y < 720 - INSET; y++) {
        uint16_t const *row = f->fb + y * 720;
        for (int x = f->x1 + INSET; x <= f->x2 - INSET; x++) {
            note_value(d, row[x]);
        }
    }
}

// The CPU's share of the real path: the engine's 8-bit frame through a 256-entry palette into the PSRAM source
static void expand(void) {
    uint16_t       *dst = (uint16_t *)job.src;
    uint8_t const  *in  = job.idx;
    uint16_t const *lut = job.lut;
    for (int i = 0; i < I8_BYTES; i++) {
        dst[i] = lut[in[i]];
    }
}

// LVGL task, lock held (the UI service)
static void job_fn(void *arg) {
    (void)arg;
    uint32_t const t0 = (uint32_t)esp_timer_get_time();
    if (job.redraw) {
        lv_obj_invalidate(lv_screen_active()); // the back buffer holds the test picture: partial rendering would show it
    } else {
        if (job.flip) {
            expand();
        }
        job.expand_us = (uint32_t)esp_timer_get_time() - t0;
        job.status    = display_vbench_frame(job.src, job.flip, &job.frame);
        if (job.status == VBENCH_OK && job.scan) {
            scan_window(&job.frame, &job.seen);
        }
    }
    job.job_us = (uint32_t)esp_timer_get_time() - t0;
    xSemaphoreGive(job.done);
}

static bool run_job(void) {
    xSemaphoreTake(job.done, 0);
    job.running = true;
    if (!lichen_sys_ui_call(job_fn, NULL, JOB_WAIT_MS)) {
        job.running = false;
        return false;
    }
    bool const ok = xSemaphoreTake(job.done, pdMS_TO_TICKS(JOB_WAIT_MS)) == pdTRUE;
    job.running = !ok;
    return ok;
}

typedef struct {
    uint64_t sum[5]; // ppa total, driver call, expand, write target, whole job
    uint32_t max[5];
    uint32_t done;
} totals_t;

static void add_frame(totals_t *t) {
    uint32_t const v[5] = {job.frame.total_us, job.frame.call_us, job.expand_us, job.frame.target_us, job.job_us};
    for (int i = 0; i < 5; i++) {
        t->sum[i] += v[i];
        t->max[i] = v[i] > t->max[i] ? v[i] : t->max[i];
    }
    t->done++;
}

static void line(char const *what, totals_t const *t, int i) {
    console_reply("vbench: %s avg %u max %u us", what, (unsigned)(t->sum[i] / (t->done ? t->done : 1)), (unsigned)t->max[i]);
}

static void report(unsigned n, totals_t const *t, uint64_t wall_us) {
    uint32_t refresh = 0;
    display_refresh_dhz(&refresh);
    unsigned const done = t->done ? t->done : 1;
    console_reply("vbench %s: %u of %u frames, 320x200 -> 720x537 (36/16 x 43/16), panel columns %d..%d, refresh %u.%u Hz",
                  job.flip ? "flip, UPPER bound (expand + swap + dirty carry)" : "plain, LOWER bound (PPA only)",
                  (unsigned)t->done, n, (int)job.frame.x1, (int)job.frame.x2, (unsigned)(refresh / 10), (unsigned)(refresh % 10));
    line("ppa to done", t, 0);
    line("driver call (write-back + invalidate)", t, 1);
    if (job.flip) {
        line("cpu expand 8->565", t, 2);
        line("swap wait + dirty carry", t, 3);
        line("whole job", t, 4);
    }
    uint64_t const per = wall_us / done; // the loop's own wall clock: the job, the hand-over and the pause between jobs
    console_reply("vbench: wall per frame %u us = %u fps (a swap per frame cannot beat the refresh)", (unsigned)per,
                  (unsigned)(per ? 1000000u / per : 0));
    char vals[DISTINCT_MAX * 5 + 1] = "";
    for (unsigned i = 0; i < job.seen.n; i++) {
        size_t len = strlen(vals);
        snprintf(vals + len, sizeof(vals) - len, "%s%04X", i ? " " : "", (unsigned)job.seen.v[i]);
    }
    console_reply("vbench: %u%s distinct pixel values (%s): %s", job.seen.n, job.seen.more ? "+" : "", vals,
                  job.seen.n >= 3 ? "blended, a filter" : job.seen.n == 2 ? "nearest" : "nothing written");
}

static char const *fail_text(vbench_status_t s) {
    return s == VBENCH_TIMEOUT   ? "no PPA done (engine now on cpu, source kept)"
           : s == VBENCH_REFUSED ? "the driver refused the transfer"
           : s == VBENCH_NO_SWAP ? "one frame buffer, no swap"
                                 : "no PPA (engine on cpu?)";
}

typedef struct {
    totals_t totals;
    bool     stuck; // a job did not answer: it may still use the source and the job state
} loop_t;

static vbench_status_t frames(unsigned n, loop_t *l, uint64_t *wall_us) {
    vbench_status_t s  = VBENCH_OK;
    int64_t const   t0 = esp_timer_get_time();
    for (unsigned i = 0; i < n && s == VBENCH_OK; i++) {
        if (!job.flip) {
            fill_pattern((uint16_t *)job.src);
        }
        job.scan = i == n - 1;
        if (!run_job()) {
            console_reply("vbench: the LVGL task did not answer in %d ms", JOB_WAIT_MS);
            l->stuck = true;
            return s;
        }
        s = job.status;
        if (s == VBENCH_OK) {
            add_frame(&l->totals);
        }
        vTaskDelay(1); // LVGL's own frames (the clock) run in between, as they would beside the video
    }
    *wall_us = (uint64_t)(esp_timer_get_time() - t0);
    return s;
}

// The last redraw only when no earlier job can still be queued or running: wait for it once more, else leave it
static void finish(loop_t const *l) {
    if (l->stuck && xSemaphoreTake(job.done, pdMS_TO_TICKS(JOB_WAIT_MS)) != pdTRUE) {
        console_reply("vbench: the job is still queued, no redraw (the picture is stale until the next one)");
        return;
    }
    job.running = false;
    job.redraw  = true;
    run_job();
}

static void run(unsigned n, bool flip) {
    uint16_t *src = heap_caps_aligned_alloc(64, SRC_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t  *idx = flip ? heap_caps_malloc(I8_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!src || (flip && !idx)) {
        console_reply("vbench: no PSRAM for the source");
        heap_caps_free(src);
        return;
    }
    if (flip) {
        fill_indexes(idx);
    }
    job.seen   = (distinct_t){0};
    job.src    = src;
    job.idx    = idx;
    job.flip   = flip;
    job.redraw = false;
    loop_t   l = {0};
    uint64_t wall_us = 0;
    vbench_status_t s = frames(n, &l, &wall_us);
    if (s != VBENCH_OK) {
        console_reply("vbench: stopped after %u frames: %s", (unsigned)l.totals.done, fail_text(s));
    } else if (!l.stuck) {
        report(n, &l.totals, wall_us);
    }
    finish(&l);
    if (!l.stuck && s != VBENCH_TIMEOUT) { // a timed-out transfer or a late job may still read them
        heap_caps_free(src);
        heap_caps_free(idx);
    }
}

static void cmd_vbench(char const *args) {
    if (job.running) {
        console_reply("vbench: the last job has not finished on the LVGL task yet");
        return;
    }
    int n = atoi(args);
    run(n <= 0 ? VBENCH_DEFAULT : n > VBENCH_MAX ? VBENCH_MAX : (unsigned)n, strstr(args, "flip") != NULL);
}

void vbench_cmd_register(void) {
    job.done = xSemaphoreCreateBinary();
    if (!job.done) {
        return;
    }
    console_register_dev("vbench",
                         "vbench [n] [flip] (#558: n PPA frames of the video path; plain = lower bound, the test "
                         "picture may flash; flip = also expand + swap + carry. See fw mem for DMA)",
                         cmd_vbench);
}

#endif
