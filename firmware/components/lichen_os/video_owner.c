// The OS video surface's owner record and "fw video" (#561, part of #527). board/display.c holds the hardware side (the
// flush filter, the pump, the PPA); this file is who owns it and the memory: two source buffers per owner, from the heap, not
// lichen_alloc, so no app's block book or the loader's sweep maps know them. Everything here runs under the UI service's lock,
// which is taken per call and never held across frames.
#include "video_owner.h"

#include "display.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lichen_sys_ui.h"
#include "esp_attr.h"
#include "psram_speed.h"

#include <string.h>

static char const TAG[] = "video";

#define LOCK_WAIT_MS 2000
#define LINE         64 // the cache line: sources start on one and span whole lines

typedef struct {
    bool                held, reclaimed;
    lichen_ctx_t        ctx;
    void               *src[2];
    void               *parked[2]; // released while the PPA may still read them; freed by the next call that finds it idle
    video_reclaim_fn    cb;
    void               *arg;
    display_video_reclaim_t reason;
} owner_t;

EXT_RAM_BSS_ATTR static owner_t o; // under the UI lock (the lock-free present reads held / reclaimed / src, racy by design)

static size_t src_bytes(uint32_t w, uint32_t h) {
    return ((size_t)w * h * 2 + LINE - 1) / LINE * LINE;
}

// Frees sources a hung PPA kept alive once its done was seen. Under the lock.
static void reap_parked(void) {
    if (o.parked[0] && display_vsurf_ppa_idle_locked()) {
        heap_caps_free(o.parked[0]);
        heap_caps_free(o.parked[1]);
        o.parked[0] = o.parked[1] = NULL;
    }
}

static void on_reclaimed(void *arg, display_video_reclaim_t why) {
    (void)arg;
    o.reclaimed = true;
    o.reason    = why;
    if (o.cb) {
        o.cb(o.arg, why);
    }
}

display_video_refusal_t video_owner_acquire(lichen_ctx_t ctx, uint32_t w, uint32_t h, uint32_t want_x_milli,
                                            uint32_t want_y_milli, video_reclaim_fn on_reclaim, void *arg, void *src[2]) {
    if (!lichen_sys_ui_lock(LOCK_WAIT_MS)) {
        return DISPLAY_VIDEO_REFUSE_UI;
    }
    reap_parked();
    display_video_geom_t    g;
    display_video_refusal_t r = o.held ? DISPLAY_VIDEO_REFUSE_OWNED
                                       : display_vsurf_check_locked(w, h, want_x_milli, want_y_milli, psram_speed_running(), true, &g);
    if (r == DISPLAY_VIDEO_REFUSE_NONE && o.parked[0]) {
        r = DISPLAY_VIDEO_REFUSE_ENGINE; // the PPA may still read the last owner's sources (hung): no new surface yet
    }
    if (r == DISPLAY_VIDEO_REFUSE_NONE) { // the verdict first: nothing is allocated for a refusal
        size_t const bytes = src_bytes(w, h); // the geometry bounds it: the output fits the panel at a scale of at least 1/16
        void        *s[2]  = {heap_caps_aligned_calloc(LINE, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), NULL};
        s[1]               = s[0] ? heap_caps_aligned_calloc(LINE, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        if (!s[0] || !s[1]) {
            r = DISPLAY_VIDEO_REFUSE_MEM;
        } else {
            for (int i = 0; i < 2; i++) {
                esp_cache_msync(s[i], bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED); // black, written back
            }
            r = display_vsurf_acquire_locked(s, w, h, want_x_milli, want_y_milli, psram_speed_running(), on_reclaimed, NULL);
        }
        if (r == DISPLAY_VIDEO_REFUSE_NONE) {
            o      = (owner_t){.held = true, .ctx = ctx, .src = {s[0], s[1]}, .cb = on_reclaim, .arg = arg};
            src[0] = s[0];
            src[1] = s[1];
        } else {
            heap_caps_free(s[0]);
            heap_caps_free(s[1]);
        }
    }
    lichen_sys_ui_unlock();
    return r;
}

bool video_owner_present(lichen_ctx_t ctx, unsigned idx) {
    if (!o.held || o.reclaimed || o.ctx != ctx) {
        return false;
    }
    return display_vsurf_present(idx);
}

bool video_owner_holds(lichen_ctx_t ctx) {
    return o.held && o.ctx == ctx;
}

// Under the lock. The display side waits out a PPA transfer before it says the sources are free; if one may still run they
// are parked, not freed (no tag-ignore): they are OS memory, so keeping them is safe, only late.
static void release_locked(void) {
    bool const idle = display_vsurf_release_locked();
    void      *s0 = o.src[0], *s1 = o.src[1];
    // The record goes first: a present or holds() racing on another task then finds nothing to read before the free
    o = (owner_t){.parked = {o.parked[0], o.parked[1]}};
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (idle) {
        heap_caps_free(s0);
        heap_caps_free(s1);
    } else {
        ESP_LOGW(TAG, "release: the PPA may still read the sources, kept until its done");
        o.parked[0] = s0;
        o.parked[1] = s1;
    }
}

static bool release_if(bool any, lichen_ctx_t ctx) {
    if (!lichen_sys_ui_lock(LOCK_WAIT_MS)) {
        return false;
    }
    reap_parked();
    if (o.held && (any || o.ctx == ctx)) {
        release_locked();
    }
    lichen_sys_ui_unlock();
    return true;
}

bool video_owner_release(lichen_ctx_t ctx) {
    return release_if(false, ctx);
}

bool video_owner_release_ctx(lichen_ctx_t ctx) {
    return ctx == LICHEN_CTX_NONE || release_if(false, ctx);
}

bool video_owner_reclaim(display_video_reclaim_t why) {
    if (!lichen_sys_ui_lock(LOCK_WAIT_MS)) {
        return false;
    }
    bool const ok = display_vsurf_reclaim_locked(why);
    lichen_sys_ui_unlock();
    return ok;
}

#if CONFIG_LICHEN_DEV_COMMANDS
void video_owner_stats(video_owner_stats_t *out) {
    display_vsurf_stats_t d;
    display_vsurf_stats(&d);
    *out = (video_owner_stats_t){.held           = o.held,
                                 .reclaimed      = o.reclaimed,
                                 .parked         = o.parked[0] != NULL,
                                 .ctx            = o.ctx,
                                 .state          = d.state,
                                 .reason         = d.reason,
                                 .acquires       = d.acquires,
                                 .reclaims       = d.reclaims,
                                 .presented      = d.presented,
                                 .frames_dropped = d.frames_dropped,
                                 .strips_dropped = d.strips_dropped,
                                 .bands_lost     = d.bands_lost,
                                 .present_max_us = d.present_max_us,
                                 .queued         = d.queued};
}
#endif

// ---- fw video (developer mode) ---------------------------------------------------------------------------------------
#if CONFIG_LICHEN_DEV_COMMANDS

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_console.h"

#include <stdlib.h>

#define TEST_W        320
#define TEST_H        200
#define RUN_DEFAULT   120
#define RUN_MAX       1200
#define SCALE_X_MILLI 2250 // 320 -> 720 (36/16)
#define SCALE_Y_MILLI 2700 // 200 -> 537 (43/16)

EXT_RAM_BSS_ATTR static void *g_src[2]; // the console's own holding, valid while it holds the surface
static bool  g_have;

// Vertical colour stripes that walk right and a white line that walks down: a torn frame breaks the line
static void paint(uint16_t *dst, uint32_t frame) {
    static uint16_t const pal[6] = {0xF800, 0xFFE0, 0x07E0, 0x07FF, 0x001F, 0xF81F};
    uint16_t              row[TEST_W];
    for (uint32_t x = 0; x < TEST_W; x++) {
        row[x] = pal[((x + frame * 3) / 20) % 6];
    }
    uint32_t const line = (frame * 2) % TEST_H;
    for (uint32_t y = 0; y < TEST_H; y++) {
        uint16_t *d = dst + y * TEST_W;
        if (y >= line && y < line + 4) {
            memset(d, 0xFF, TEST_W * 2);
        } else {
            memcpy(d, row, sizeof(row));
        }
    }
}

static void say_state(void) {
    video_owner_stats_t s;
    video_owner_stats(&s);
    console_reply("video: %s%s, presented %u, frames dropped %u, strips dropped %u, queued bands %u (lost %u)",
                  display_video_state_name(s.state), s.reclaimed ? " (reclaimed, not released)" : "", (unsigned)s.presented,
                  (unsigned)s.frames_dropped, (unsigned)s.strips_dropped, (unsigned)s.queued, (unsigned)s.bands_lost);
    console_reply("video: present max %u us, acquires %u, reclaims %u, last reclaim %s, held %s%s", (unsigned)s.present_max_us,
                  (unsigned)s.acquires, (unsigned)s.reclaims, display_video_reclaim_name(s.reason), s.held ? "yes" : "no",
                  s.parked ? ", sources parked (PPA busy)" : "");
}

static bool take(void) {
    if (g_have) {
        return true;
    }
    display_video_refusal_t const r =
        video_owner_acquire(LICHEN_CTX_NONE, TEST_W, TEST_H, SCALE_X_MILLI, SCALE_Y_MILLI, NULL, NULL, g_src);
    if (r != DISPLAY_VIDEO_REFUSE_NONE) {
        console_reply("video: refused (%s)", display_video_refusal_name(r));
        return false;
    }
    g_have = true;
    return true;
}

static void let_go(void) {
    if (g_have) {
        g_have = !video_owner_release(LICHEN_CTX_NONE);
    }
}

static void run(unsigned n, bool hold) {
    if (!take()) {
        return;
    }
    int64_t const t0 = esp_timer_get_time();
    unsigned      sent = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned const buf = (i + 1) & 1; // the first frame goes into source 1 (source 0 is the black one shown until then)
        paint(g_src[buf], i);
        if (video_owner_present(LICHEN_CTX_NONE, buf)) {
            sent++;
        } else {
            video_owner_stats_t s;
            video_owner_stats(&s);
            if (s.reclaimed || s.state != DISPLAY_VIDEO_ACTIVE) {
                console_reply("video: stopped at frame %u, the screen was taken back (%s)", i, display_video_reclaim_name(s.reason));
                break;
            }
        }
    }
    unsigned const ms = (unsigned)((esp_timer_get_time() - t0) / 1000);
    console_reply("video run: %u of %u frames handed over in %u ms = %u fps", sent, n, ms, ms ? sent * 1000 / ms : 0);
    vTaskDelay(pdMS_TO_TICKS(100)); // the last frame is drawn
    say_state();
    if (!hold) {
        let_go();
    }
}

static bool reclaim_reason(char const *w, display_video_reclaim_t *out) {
    for (int r = DISPLAY_VIDEO_RECLAIM_DIALOG; r < DISPLAY_VIDEO_RECLAIM_COUNT; r++) {
        if (strcmp(w, display_video_reclaim_name((display_video_reclaim_t)r)) == 0) {
            *out = (display_video_reclaim_t)r;
            return true;
        }
    }
    return false;
}

static void cmd_video(char const *args) {
    while (*args == ' ') {
        args++;
    }
    display_video_reclaim_t why;
    if (!*args || strncmp(args, "status", 6) == 0) {
        say_state();
    } else if (strncmp(args, "run", 3) == 0) {
        int n = atoi(args + 3);
        run(n <= 0 ? RUN_DEFAULT : n > RUN_MAX ? RUN_MAX : (unsigned)n, strstr(args, "hold") != NULL);
    } else if (strcmp(args, "hold") == 0) {
        if (take()) {
            console_reply("video: held, paused (black picture; the bars stay live)");
        }
    } else if (strcmp(args, "present") == 0) {
        if (take()) {
            static uint32_t frame;
            unsigned const  buf = (frame + 1) & 1;
            paint(g_src[buf], frame);
            console_reply("video: present %s", video_owner_present(LICHEN_CTX_NONE, buf) ? "ok" : "dropped");
            frame++;
        }
    } else if (strcmp(args, "release") == 0) {
        let_go();
        console_reply("video: %s", g_have ? "the UI lock was not free, still held" : "released");
    } else if (strncmp(args, "reclaim ", 8) == 0 && reclaim_reason(args + 8, &why)) {
        console_reply("video: reclaim %s %s", args + 8, video_owner_reclaim(why) ? "done" : "refused (not active)");
    } else {
        console_reply("usage: video [status] | run [frames] [hold] | hold | present | release | reclaim "
                      "<dialog|thermal|card|escape|engine>");
    }
}

void video_owner_register(void) {
    console_register_dev("video",
                         "video [status] | run [frames] [hold] | hold | present | release | reclaim <why> (#561: the OS "
                         "video surface with a test pattern; DMA gate and fw mem as for vbench)",
                         cmd_video);
}

#endif
