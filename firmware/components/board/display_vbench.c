// "fw vbench [n]" (#558 spike, developer builds only): n scale + rotate frames of the planned DOOM video path (#527), the
// time per frame, and the PPA's scaling filter read from the pixels (the DMA RAM is read with "fw mem" before and after). Under the
// LVGL lock for one frame at a time, never across frames. The numbers go on #527; nothing here ships in a release image.
#include "display_vbench.h"

#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH && CONFIG_LICHEN_DEV_COMMANDS

#include "display_info.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VBENCH_DEFAULT 100
#define VBENCH_MAX     600
#define SRC_BYTES      (VBENCH_SRC_W * VBENCH_SRC_H * 2)
#define DISTINCT_MAX   8

// Four quadrants of black and white split at the middle of the source, so one vertical and one horizontal edge cross
// the picture: a nearest filter leaves two values in the output, a bilinear one greys along both edges. Written whole
// before every frame, as the real path writes a new picture, so the driver's source write-back is in the time.
static void fill_pattern(uint16_t *src) {
    uint16_t *const top = src, *const bottom = src + (VBENCH_SRC_H / 2) * VBENCH_SRC_W; // built in place: no stack
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

typedef struct {
    uint16_t v[DISTINCT_MAX];
    unsigned n;
    bool     more;
} distinct_t;

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

// The distinct pixel values of the output window, read after an invalidate so no stale cache line answers for the PPA
static void scan_window(vbench_frame_t const *f, distinct_t *d) {
    size_t const row_bytes = 720 * sizeof(uint16_t);
    esp_cache_msync((void *)f->fb, 720 * row_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    for (int y = 0; y < 720; y++) {
        uint16_t const *row = f->fb + y * 720;
        for (int x = f->x1; x <= f->x2; x++) {
            note_value(d, row[x]);
        }
    }
}

typedef struct {
    uint64_t sum_total, sum_call;
    uint32_t max_total, max_call, done;
    vbench_status_t fail;
} totals_t;

static void add_frame(totals_t *t, vbench_frame_t const *f) {
    t->sum_total += f->total_us;
    t->sum_call += f->call_us;
    t->max_total = f->total_us > t->max_total ? f->total_us : t->max_total;
    t->max_call  = f->call_us > t->max_call ? f->call_us : t->max_call;
    t->done++;
}

static void report(unsigned n, totals_t const *t, distinct_t const *d, vbench_frame_t const *last) {
    uint32_t refresh = 0;
    display_refresh_dhz(&refresh);
    unsigned const done = t->done ? t->done : 1;
    console_reply("vbench: %u of %u frames, 320x200 -> 720x537 (36/16 x 43/16) turned 90 into panel columns %d..%d, panel "
                  "refresh %u.%u Hz",
                  (unsigned)t->done, n, (int)last->x1, (int)last->x2, (unsigned)(refresh / 10), (unsigned)(refresh % 10));
    console_reply("vbench: per frame to done: avg %u max %u us (%u fps back to back)", (unsigned)(t->sum_total / done),
                  (unsigned)t->max_total, (unsigned)(1000000ull * done / (t->sum_total ? t->sum_total : 1)));
    console_reply("vbench: driver call (source write-back + output window invalidate): avg %u max %u us",
                  (unsigned)(t->sum_call / done), (unsigned)t->max_call);
    char vals[DISTINCT_MAX * 7 + 4] = "";
    for (unsigned i = 0; i < d->n; i++) {
        size_t len = strlen(vals);
        snprintf(vals + len, sizeof(vals) - len, "%s%04X", i ? " " : "", (unsigned)d->v[i]);
    }
    console_reply("vbench: output has %u%s distinct pixel values (%s): %s", d->n, d->more ? "+" : "", vals,
                  d->n == 2 ? "nearest, no blending" : "blended, a filter");
}

static void run(unsigned n) {
    uint16_t *src = heap_caps_aligned_alloc(64, SRC_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!src) {
        console_reply("vbench: no PSRAM for the %u KB source", SRC_BYTES >> 10);
        return;
    }
    totals_t       t = {0};
    vbench_frame_t last = {0};
    distinct_t     d = {0};
    for (unsigned i = 0; i < n && t.fail == VBENCH_OK; i++) {
        fill_pattern(src);
        lvgl_port_lock(0);
        vbench_frame_t f = {0};
        t.fail           = display_vbench_frame(src, &f);
        if (t.fail == VBENCH_OK) {
            add_frame(&t, &f);
            last = f;
            if (i == n - 1) {
                scan_window(&f, &d);
            }
        }
        lvgl_port_unlock();
        vTaskDelay(1); // LVGL's own frames (the clock) run in between, as they would beside the video
    }
    if (t.fail == VBENCH_OK) {
        report(n, &t, &d, &last);
    } else {
        console_reply("vbench: stopped after %u frames: %s", (unsigned)t.done,
                      t.fail == VBENCH_TIMEOUT ? "no PPA done (engine now on cpu, source kept)"
                      : t.fail == VBENCH_REFUSED ? "the driver refused the transfer"
                                                 : "no PPA available");
    }
    // The back buffer holds the test picture: redraw everything (partial rendering would show it at the next swap)
    lvgl_port_lock(0);
    lv_obj_invalidate(lv_screen_active());
    lvgl_port_unlock();
    vTaskDelay(pdMS_TO_TICKS(500));
    if (t.fail != VBENCH_TIMEOUT) {
        heap_caps_free(src); // a timed-out transfer may still read it: leaked on purpose, the engine is on the CPU now
    }
}

static void cmd_vbench(char const *args) {
    int n = atoi(args);
    run(n <= 0 ? VBENCH_DEFAULT : n > VBENCH_MAX ? VBENCH_MAX : (unsigned)n);
}

void display_vbench_register(void) {
    console_register_dev("vbench",
                         "vbench [n] (#558: n scale + rotate PPA frames of the video path into the back buffer, "
                         "default 100, max 600; time per frame and the scaling filter; "
                         "read DMA RAM with fw mem before and after. "
                         "Redraws the whole screen after; needs the ppa-async engine)",
                         cmd_vbench);
}

#endif
