// "fw vbench [n]" (#558 spike, developer builds only): n scale + rotate PPA frames of the planned DOOM video path (#527),
// the time per frame, and the PPA's scaling filter read from the pixels. Each frame is one job on the LVGL task through
// the UI service (display_vbench_frame needs the PPA client's core and the lock), the console task only waits for it,
// so the LVGL lock is never held across frames. The numbers go on #527; nothing here ships in a release image.
#include "vbench_cmd.h"

#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH && CONFIG_LICHEN_DEV_COMMANDS

#include "display_info.h"
#include "display_vbench.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
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
    bool              scan, redraw;
    vbench_status_t   status;
    vbench_frame_t    frame;
    distinct_t        seen;
    volatile bool     running;
} job;

// Four quadrants of black and white split at the middle of the source, so one vertical and one horizontal edge cross
// the picture: a nearest filter leaves two values in the output, a bilinear one greys along both edges. Written whole
// before every frame, as the real path writes a new picture, so the driver's source write-back is in the time. Built in
// place: no stack.
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

// LVGL task, lock held (the UI service)
static void job_fn(void *arg) {
    (void)arg;
    if (job.redraw) {
        lv_obj_invalidate(lv_screen_active()); // the back buffer holds the test picture: partial rendering would show it
    } else {
        job.status = display_vbench_frame(job.src, &job.frame);
        if (job.status == VBENCH_OK && job.scan) {
            scan_window(&job.frame, &job.seen);
        }
    }
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
    uint64_t sum_total, sum_call;
    uint32_t max_total, max_call, done;
} totals_t;

static void add_frame(totals_t *t, vbench_frame_t const *f) {
    t->sum_total += f->total_us;
    t->sum_call += f->call_us;
    t->max_total = f->total_us > t->max_total ? f->total_us : t->max_total;
    t->max_call  = f->call_us > t->max_call ? f->call_us : t->max_call;
    t->done++;
}

static char const *filter_verdict(unsigned n) {
    return n >= 3 ? "blended, a filter" : n == 2 ? "nearest, no blending" : "nothing written to the window";
}

static void report(unsigned n, totals_t const *t) {
    uint32_t refresh = 0;
    display_refresh_dhz(&refresh);
    unsigned const done = t->done ? t->done : 1;
    console_reply("vbench: %u of %u frames, 320x200 -> 720x537 (36/16 x 43/16) turned 90 into panel columns %d..%d, panel "
                  "refresh %u.%u Hz",
                  (unsigned)t->done, n, (int)job.frame.x1, (int)job.frame.x2, (unsigned)(refresh / 10), (unsigned)(refresh % 10));
    console_reply("vbench: per frame to done: avg %u max %u us (%u fps back to back)", (unsigned)(t->sum_total / done),
                  (unsigned)t->max_total, (unsigned)(1000000ull * done / (t->sum_total ? t->sum_total : 1)));
    console_reply("vbench: driver call (source write-back + output window invalidate): avg %u max %u us",
                  (unsigned)(t->sum_call / done), (unsigned)t->max_call);
    char vals[DISTINCT_MAX * 5 + 1] = "";
    for (unsigned i = 0; i < job.seen.n; i++) {
        size_t len = strlen(vals);
        snprintf(vals + len, sizeof(vals) - len, "%s%04X", i ? " " : "", (unsigned)job.seen.v[i]);
    }
    console_reply("vbench: output has %u%s distinct pixel values (%s): %s", job.seen.n, job.seen.more ? "+" : "", vals,
                  filter_verdict(job.seen.n));
}

static char const *fail_text(vbench_status_t s) {
    return s == VBENCH_TIMEOUT   ? "no PPA done (engine now on cpu, source kept)"
           : s == VBENCH_REFUSED ? "the driver refused the transfer"
                                 : "no PPA available (engine on cpu?)";
}

static void run(unsigned n) {
    uint16_t *src = heap_caps_aligned_alloc(64, SRC_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!src) {
        console_reply("vbench: no PSRAM for the %u KB source", SRC_BYTES >> 10);
        return;
    }
    totals_t t        = {0};
    bool     leaked   = false;
    job.seen          = (distinct_t){0};
    job.src           = src;
    job.redraw        = false;
    vbench_status_t s = VBENCH_OK;
    for (unsigned i = 0; i < n && s == VBENCH_OK; i++) {
        fill_pattern(src);
        job.scan = i == n - 1;
        if (!run_job()) {
            console_reply("vbench: the LVGL task did not answer in %d ms, stopped after %u frames", JOB_WAIT_MS, (unsigned)t.done);
            leaked = true; // the job may still read the source
            break;
        }
        s = job.status;
        if (s == VBENCH_OK) {
            add_frame(&t, &job.frame);
        }
        vTaskDelay(1); // LVGL's own frames (the clock) run in between, as they would beside the video
    }
    if (s == VBENCH_OK && !leaked) {
        report(n, &t);
    } else if (s != VBENCH_OK) {
        console_reply("vbench: stopped after %u frames: %s", (unsigned)t.done, fail_text(s));
        leaked = leaked || s == VBENCH_TIMEOUT; // a timed-out transfer may still read it
    }
    job.redraw = true;
    run_job();
    if (!leaked) {
        heap_caps_free(src);
    }
}

static void cmd_vbench(char const *args) {
    if (job.running) {
        console_reply("vbench: the last job has not finished on the LVGL task yet");
        return;
    }
    int n = atoi(args);
    run(n <= 0 ? VBENCH_DEFAULT : n > VBENCH_MAX ? VBENCH_MAX : (unsigned)n);
}

void vbench_cmd_register(void) {
    job.done = xSemaphoreCreateBinary();
    if (!job.done) {
        return;
    }
    console_register_dev("vbench",
                         "vbench [n] (#558: n scale + rotate PPA frames of the video path into the back buffer, "
                         "default 100, max 600; time per frame and the scaling filter; read DMA RAM with fw mem "
                         "before and after. Redraws the whole screen after; needs the ppa-async engine)",
                         cmd_vbench);
}

#endif
