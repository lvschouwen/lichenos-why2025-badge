#include "display.h"
#include "display_timing.h"

#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7703.h"
#include "esp_ldo_regulator.h"
#include "esp_clk_tree.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "soc/hp_sys_clkrst_struct.h"
#include "st7703_bono.h"

#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH
#include "display_engine.h"
#include "display_flip.h"
#include "display_rot.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_lcd_panel_interface.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdint.h>
#include <string.h>
#include "driver/ppa.h"
#endif
#include "display_info.h"
#include "display_vbench.h"

static char const TAG[] = "display";

#define LCD_H_RES                   720
#define LCD_V_RES                   720
#define LCD_IO_RST                  17
#define DSI_PHY_LDO_CHAN            3 // LDO_VO3 feeds VDD_MIPI_DPHY
#define DSI_PHY_LDO_MV              2500
#define DSI_LANES                   2
#define DSI_LANE_MBPS               1000
#define DRAW_BUFFER_LINES           80
#define LVGL_TASK_STACK            10240 // measured ~6.1 KiB used on home, Contacts, Settings (#26): 40 % headroom

static st7703_lcd_init_cmd_t const init_cmds[] = CUSTOM_INIT_CMDS();

static esp_lcd_panel_handle_t lcd_panel;

#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH
// Direct flush (#95). The panel sits on its side, so every 720x80 strip LVGL renders has to be turned by 90
// degrees. esp_lvgl_port's way costs five PSRAM passes per strip: LVGL writes the strip, the port reads it
// and writes it turned into its own rotation buffer, then the DPI driver's DMA2D reads that and writes it
// into the frame buffer. Here the strip is turned straight into the frame buffer: three passes, no rotation
// buffer, no DMA2D copy, and flush_ready comes from this callback instead of the DMA2D done interrupt.
//
// Tear-free (#467): two frame buffers (num_fbs = 2). The strips of a frame go into the back buffer, the one the
// DMA is not reading. After the last strip the switch is requested and the flush returns at once; the first strip
// of the next frame resolves it (bounded wait, usually already proven) and copies the areas the earlier frame drew
// into the new back buffer, so both hold the same picture again (PARTIAL rendering only redraws what changed). The
// state machine is display_flip.c; here are its hardware hooks.
// How the IDF 6.1 DPI driver swaps (esp_lcd/dsi/esp_lcd_panel_dpi.c, read, not changed):
// - draw_bitmap with a pointer inside fbs[i] copies nothing: it writes back the cache of the rows it names, sets
//   cur_fb_index = i and calls on_color_trans_done at once (dpi_panel_draw_bitmap_2d, lines 613-637).
// - The DMA reads one whole frame buffer per transfer; its done interrupt (mipi_dsi_dma_trans_done_cb, lines
//   83-103) reads cur_fb_index, restarts on that buffer's link list and then calls on_frame_buf_complete. So the
//   switch takes effect at the next end of frame, and the first on_frame_buf_complete after the index was set
//   says the old buffer is no longer read. That ISR is not IRAM (DW_GDMA_ISR_IRAM_SAFE off), so neither is ours.
// - The interrupt reads the index a few instructions before the callback, so the first callback after the request
//   may have restarted on the old buffer: display_swap_proven() then wants a later stamp or a second callback.
// The driver starts on fbs[0] (dpi_panel_init) and callocs both, so both start equal (black).
#define FB_BYTES        (LCD_H_RES * LCD_V_RES * 2)
#define SWAP_GUARD_US   200 // the ISR's index read to callback stamp is well under this
#define SWAP_TIMEOUT_MS 100 // about six frames at 62.45 Hz: a stalled scan-out costs this once per frame, no more
#define FLIP_LOG_SWAPS  600 // developer builds: one stats line per ~10 s of continuous redraw

static uint16_t         *panel_fbs[2];
EXT_RAM_BSS_ATTR static display_flip_t flip; // LVGL task only
static SemaphoreHandle_t fb_done;            // given by on_fb_done at every end of frame
static StaticSemaphore_t fb_done_buf;
// Written by on_fb_done, stamp first; a reader that sees the count but an older stamp only waits longer
static uint32_t volatile fb_done_us; // low 32 bits of esp_timer
static uint32_t volatile fb_done_count;
static uint32_t          swap_misses;
EXT_RAM_BSS_ATTR static display_engine_t engine; // LVGL task; the registry setter under the LVGL lock
static lv_display_t *direct_disp;                // set once the direct flush runs (the PPA interrupt readies it)

// The port's sw_rotate=0 path "rotates" by calling swap_xy/mirror on the panel, which on the ST7703 means
// MADCTL writes over DBI while the panel is in video mode. Hand it a control panel that does nothing instead.
static esp_err_t no_swap_xy(esp_lcd_panel_t *panel, bool swap) {
    (void)panel;
    (void)swap;
    return ESP_OK;
}

static esp_err_t no_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y) {
    (void)panel;
    (void)mirror_x;
    (void)mirror_y;
    return ESP_OK;
}

static esp_lcd_panel_t no_hw_rotate = {.swap_xy = no_swap_xy, .mirror = no_mirror};

// DMA done ISR, once per scanned frame: kept to a stamp, a count and a give
static bool on_fb_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *ctx) {
    (void)panel;
    (void)edata;
    (void)ctx;
    fb_done_us = (uint32_t)esp_timer_get_time();
    fb_done_count++;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(fb_done, &woken);
    return woken == pdTRUE;
}

static display_frame_mark_t frame_mark(void) {
    display_frame_mark_t const m = {.count = fb_done_count, .us = fb_done_us};
    return m;
}

static uint32_t flip_now_us(void *ctx) {
    (void)ctx;
    return (uint32_t)esp_timer_get_time();
}

// Row 0 is the cache write-back the driver does for a 1x1 area; every strip was written back already
static bool flip_request(void *ctx, unsigned fb) {
    (void)ctx;
    esp_err_t const err = esp_lcd_panel_draw_bitmap(lcd_panel, 0, 0, 1, 1, panel_fbs[fb]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "frame buffer swap: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static uint32_t flip_frame_count(void *ctx) {
    (void)ctx;
    return fb_done_count;
}

// Bounded: a stalled scan-out (flash erase, panel off) costs SWAP_TIMEOUT_MS and a log line, never a hung UI. After an
// idle spell the proof is already there and nothing waits. The semaphore is not drained at the request, so a give
// left from an earlier frame returns at once and the loop goes round once more: harmless, the marks decide.
static bool flip_wait(void *ctx, display_frame_mark_t req) {
    (void)ctx;
    TickType_t const until = xTaskGetTickCount() + pdMS_TO_TICKS(SWAP_TIMEOUT_MS);
    while (!display_swap_proven(req, frame_mark(), SWAP_GUARD_US)) {
        TickType_t const left = until - xTaskGetTickCount();
        if ((int32_t)left <= 0 || xSemaphoreTake(fb_done, left) != pdTRUE) {
            if ((swap_misses++ % 100) == 0) {
                ESP_LOGW(TAG, "swap: no end of frame in %d ms (%u)", SWAP_TIMEOUT_MS, (unsigned)swap_misses);
            }
            return false;
        }
    }
    return true;
}

// Written back at once: a PPA transfer invalidates its destination rows before it writes, which would drop dirty
// lines of this copy, and the scan-out reads PSRAM, not the cache.
static void flip_copy(void *ctx, unsigned dst, unsigned src, display_rect_t r) {
    (void)ctx;
    display_copy_rect(panel_fbs[dst], panel_fbs[src], LCD_H_RES, r);
    esp_cache_msync(panel_fbs[dst] + r.y1 * LCD_H_RES, (size_t)(r.y2 - r.y1 + 1) * LCD_H_RES * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static display_flip_ops_t const flip_ops = {flip_request, flip_frame_count, flip_wait, flip_copy, flip_now_us, NULL};

// Bench figures (#467, #468): the time spent waiting for the swap proof, copying back and turning the strips with the
// engine in use, developer builds only. One run of scrolling per engine gives comparable lines.
static void flip_log_stats(void) {
#if CONFIG_LICHEN_DEV_COMMANDS
    display_flip_stats_t *const   st = &flip.stats;
    display_engine_stats_t *const es = &engine.stats;
    uint32_t const                n  = st->swaps + st->timeouts;
    if (n < FLIP_LOG_SWAPS) {
        return;
    }
    uint32_t const strips = es->strips ? es->strips : 1;
    ESP_LOGI(TAG,
             "flip: %u swaps, %u timeouts, %u refused; wait avg %u max %u us; copy avg %u max %u us; "
             "%s rotate avg %u max %u us, %u strips, %u fallbacks (%u late), %u re-arms",
             (unsigned)st->swaps, (unsigned)st->timeouts, (unsigned)st->refusals, (unsigned)(st->wait_us / n),
             (unsigned)st->wait_max_us, (unsigned)(st->copy_us / n), (unsigned)st->copy_max_us,
             display_engine_name(display_engine_active(&engine)), (unsigned)(es->rotate_us / strips),
             (unsigned)es->rotate_max_us, (unsigned)es->strips, (unsigned)es->fallbacks,
             (unsigned)engine.late_fallbacks, (unsigned)engine.rearms);
    memset(st, 0, sizeof(*st));
    memset(es, 0, sizeof(*es));
#endif
}

// The turned strip is LCD_V_RES rows of h pixels: write back the whole row span in one msync, as the DPI driver's
// own CPU copy path does (only dirty lines cost a PSRAM write). Written back before it returns, so a PPA transfer
// that invalidates these rows later (any engine switch included) drops no CPU data.
static void cpu_turn(void *ctx, display_strip_t const *s, unsigned fb) {
    (void)ctx;
    uint16_t *const dst_row = panel_fbs[fb] + s->blk.y1 * LCD_H_RES;
    lv_draw_sw_rotate(s->px, dst_row + s->blk.x1, s->ui.x2 - s->ui.x1 + 1, s->ui.y2 - s->ui.y1 + 1, s->stride,
                      LCD_H_RES * 2, LV_DISPLAY_ROTATION_90, LV_COLOR_FORMAT_RGB565);
    esp_cache_msync(dst_row, (size_t)(s->blk.y2 - s->blk.y1 + 1) * LCD_H_RES * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

// PSRAM ECC (CONFIG_SPIRAM_ECC_ENABLE, the open IDF bug with PPA reads from ECC PSRAM) is off in sdkconfig.defaults:
// accepted, nothing to guard (#475).
// The PPA engine (#468, #475, experiments e1/e2 of #95). What the IDF 6.1 driver does (esp_driver_ppa, read, not
// changed):
// - Cache, both modes, before the transfer and in the caller's task: the input window is written back (C2M,
//   unaligned allowed, ppa_srm.c:276) and the output window invalidated (M2C, ppa_srm.c:285); nothing after it. The
//   output window is whole rows (block_offset_y .. + block_h, full pic_w), and our turned strip spans all 720 rows,
//   so every transfer invalidates the whole frame buffer: no CPU write into it may still be dirty. cpu_turn and
//   flip_copy write back before they return, and the engine module never lets the CPU touch a frame buffer while a
//   transfer runs (a timed-out one aside, see display_engine_set), so none is, whatever engine ran before a switch.
// - Blocking mode waits for the done with portMAX_DELAY (ppa_core.c:540): a PPA that never finishes would hang the
//   LVGL task. So both engines start non-blocking (ppa_core.c:500, no wait) and wait on our own done, bounded.
// - The done callback runs from the DMA2D RX channel's EOF interrupt (ppa_transaction_done_cb, ppa_core.c:547, set
//   as on_recv_eof in ppa_srm.c:126), after the driver recycled the transaction (ppa_core.c:572; done_cb at :592),
//   so the next start cannot find the client busy. Only a successful EOF gets there: dma2d_rx_isr delivers on_recv_eof
//   only for RX_SUC_EOF (dma2d.c:363, the only EOF event unmasked, :773-775); an error EOF frees the channels
//   (:345-:358) and calls nobody. A 2D-DMA error therefore never reads as a done, it ends as a timeout below (#475: the
//   old "an error looks like a done" risk does not exist in 6.1, the driver's own TODO at ppa_core.c:116 says the
//   same, a stuck transfer). That interrupt is not IRAM
//   (DMA2D_ISR_IRAM_SAFE off), so neither is ours: a flash write holds it back, and one longer than the bound
//   below ends in a fallback to the CPU, logged once.
// - max_pending_trans_num defaults to 1: one transfer at a time, which is all the engine module ever starts.
#define PPA_DONE_TIMEOUT_MS 200 // a strip takes a few ms; a flash erase may hold the non-IRAM interrupt back longer
// A done later than that is still taken before the CPU redoes the strip (display_engine_settle): the late transfer
// must not write its old strip over pixels the CPU path drew since. Only a PPA silent for this long as well is given
// up on (hung: no PPA engine until its own done shows up).
#define PPA_DONE_LATE_MS 2000

static ppa_client_handle_t ppa_srm;  // set up at the first strip after a switch to a PPA engine, kept after
static SemaphoreHandle_t   ppa_done; // given by on_ppa_done once per current transfer
static uint32_t volatile ppa_done_us;
static bool volatile ppa_ready_in_isr; // this transfer's done readies LVGL (a background strip)
// Transfer tags (the engine module's gen): the one in flight, 0 after a timeout, and the last any done reported. The
// tag travels as the transfer's user_data (ppa_srm.c copies it into the transaction, done_cb gets it, ppa_core.c:592),
// so a late done of a transfer given up on is told apart from the current one.
static uint32_t volatile ppa_gen_current;
static uint32_t volatile ppa_gen_last_done;

static bool on_ppa_done(ppa_client_handle_t client, ppa_event_data_t *edata, void *user_data) {
    (void)client;
    (void)edata;
    uint32_t const got = (uint32_t)(uintptr_t)user_data;
    ppa_gen_last_done  = got; // a timed-out transfer's own done: the PPA is idle again (display_engine_ppa_done_seen)
    if (!display_engine_done_is_current(ppa_gen_current, got)) {
        return false; // neither readies LVGL nor counts as the done of a later transfer
    }
    ppa_done_us = (uint32_t)esp_timer_get_time();
    if (ppa_ready_in_isr) {
        ppa_ready_in_isr = false;
        lv_display_flush_ready(direct_disp); // two stores, safe from an interrupt
    }
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(ppa_done, &woken);
    return woken == pdTRUE;
}

static bool ppa_start(void *ctx, display_strip_t const *s, unsigned fb, bool ready_in_isr, uint32_t gen) {
    (void)ctx;
    if (!ppa_srm) {
        return false; // the client could not be set up (logged by ppa_up)
    }
    ppa_srm_oper_config_t const op = {
        .in.buffer          = s->px,
        .in.pic_w           = s->stride / 2,
        .in.pic_h           = s->ui.y2 - s->ui.y1 + 1,
        .in.block_w         = s->ui.x2 - s->ui.x1 + 1,
        .in.block_h         = s->ui.y2 - s->ui.y1 + 1,
        .in.srm_cm          = PPA_SRM_COLOR_MODE_RGB565,
        .out.buffer         = panel_fbs[fb],
        .out.buffer_size    = FB_BYTES,
        .out.pic_w          = LCD_H_RES,
        .out.pic_h          = LCD_V_RES,
        .out.block_offset_x = s->blk.x1,
        .out.block_offset_y = s->blk.y1,
        .out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
        .rotation_angle     = PPA_SRM_ROTATION_ANGLE_90, // counter-clockwise, as LV_DISPLAY_ROTATION_90 (the port maps them 1:1)
        .scale_x            = 1.0f,
        .scale_y            = 1.0f,
        .mode               = PPA_TRANS_MODE_NON_BLOCKING,
        .user_data          = (void *)(uintptr_t)gen,
    };
    xSemaphoreTake(ppa_done, 0); // a give that raced a timeout
    ppa_ready_in_isr    = ready_in_isr;
    ppa_gen_current     = gen; // before the start: its done may come at once
    esp_err_t const err = ppa_do_scale_rotate_mirror(ppa_srm, &op);
    if (err != ESP_OK) {
        ppa_ready_in_isr = false;
        ppa_gen_current  = 0;
        ESP_LOGE(TAG, "PPA strip %d,%d..%d,%d: %s", (int)s->ui.x1, (int)s->ui.y1, (int)s->ui.x2, (int)s->ui.y2,
                 esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool ppa_wait(void *ctx, uint32_t *done_us, bool late) {
    (void)ctx;
    if (xSemaphoreTake(ppa_done, pdMS_TO_TICKS(late ? PPA_DONE_LATE_MS : PPA_DONE_TIMEOUT_MS)) != pdTRUE) {
        if (late) {
            ppa_gen_current  = 0; // its later done is ignored (on_ppa_done)
            ppa_ready_in_isr = false;
        }
        return false;
    }
    *done_us = ppa_done_us;
    return true;
}

// The SRM client, once: its driver state is heap (internal by MALLOC_CAP_DEFAULT, plus two DMA descriptors), so a
// badge that never switches pays no heap (the drivers' code, DMA2D's IRAM interrupt included, is linked regardless).
// Called from the LVGL task, not from the registry setter: ppa_register_client acquires the DMA2D pool (ppa_core.c:361
// -> :205, dma2d_acquire_pool at dma2d.c:410), which allocates its interrupts on the calling core (dma2d.c:519). Here
// that is the LVGL task's core 1, so the done interrupt does not depend on how busy core 0 (the console) is.
static bool ppa_up(void) {
    if (ppa_srm) {
        return true;
    }
    ppa_done = ppa_done ? ppa_done : xSemaphoreCreateBinary();
    if (!ppa_done) {
        return false;
    }
    ppa_client_config_t const   cfg = {.oper_type = PPA_OPERATION_SRM};
    ppa_event_callbacks_t const cbs = {.on_trans_done = on_ppa_done};
    ppa_client_handle_t         c   = NULL;
    esp_err_t                   err = ppa_register_client(&cfg, &c);
    if (err == ESP_OK && (err = ppa_client_register_event_callbacks(c, &cbs)) != ESP_OK) {
        ppa_unregister_client(c);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PPA client: %s", esp_err_to_name(err));
        return false;
    }
    ppa_srm = c;
    return true;
}

// First strip after a switch to a PPA engine: the client, on this task's core. A failure goes back to cpu (the
// registry's read-back then says so).
static void ppa_on_demand(void) {
    display_engine_id_t const want = display_engine_active(&engine);
    if (want != DISPLAY_ENGINE_CPU && !ppa_srm && !ppa_up()) {
        ESP_LOGE(TAG, "rotate: %s -> cpu, no PPA client", display_engine_name(want));
        display_engine_select(&engine, DISPLAY_ENGINE_CPU);
    }
}

// Once per event, never per strip. A refused start or a hung PPA keeps the CPU until selected again (or for the boot);
// a late done re-arms ppa-async by itself (display_engine.h), the third one does not.
static void engine_fell_back(void *ctx, display_engine_id_t from, display_fallback_t why) {
    (void)ctx;
    static char const *const text[DISPLAY_FALLBACK_COUNT] = {
        [DISPLAY_FALLBACK_START]      = "the PPA refused the transfer",
        [DISPLAY_FALLBACK_TIMEOUT]    = "no PPA done in time (hung, cpu until reboot)",
        [DISPLAY_FALLBACK_LATE]       = "PPA done late, ppa-async again in 5 s",
        [DISPLAY_FALLBACK_LATE_FINAL] = "PPA done late again, staying on cpu this boot",
        [DISPLAY_EVENT_REARM]         = "ppa-async again after a late PPA done",
    };
    ESP_LOGW(TAG, "rotate: %s -> %s, %s", display_engine_name(from),
             why == DISPLAY_EVENT_REARM ? "ppa-async" : "cpu",
             text[why < DISPLAY_FALLBACK_COUNT ? why : DISPLAY_FALLBACK_TIMEOUT]);
}

static display_engine_ops_t const engine_ops = {cpu_turn, ppa_start, ppa_wait, engine_fell_back, flip_now_us, NULL};

static void flush_direct(lv_display_t *disp, lv_area_t const *area, uint8_t *px_map) {
    display_rect_t const  ui = {area->x1, area->y1, area->x2, area->y2};
    display_strip_t const s  = {
         .ui     = ui,
         .blk    = display_rot90_block(ui, LCD_V_RES), // UI width = panel height
         .px     = px_map,
         .stride = lv_draw_buf_width_to_stride(ui.x2 - ui.x1 + 1, LV_COLOR_FORMAT_RGB565),
    };
    bool const last = lv_display_flush_is_last(disp); // read first: flush_ready (the interrupt's too) clears it
    ppa_on_demand();
    bool const done = display_engine_flush(&engine, &flip, &s, last);
    if (last) {
        flip_log_stats();
    }
    if (done) {
        lv_display_flush_ready(disp);
    }
}

// LVGL waits here, not in its own spin on disp->flushing, before it hands over the next strip: bounded
static void flush_wait(lv_display_t *disp) {
    (void)disp;
    display_engine_settle(&engine);
}

// Frame buffers. False = fall back to the port's path (with a CPU copy instead of DMA2D, which is not enabled in this
// configuration); it draws into fbs[0], the one scanned out.
static bool direct_flush_init(esp_lcd_panel_handle_t panel) {
    void *fb0 = NULL, *fb1 = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &fb0, &fb1) != ESP_OK || !fb0 || !fb1) {
        ESP_LOGE(TAG, "direct flush: not two frame buffers");
        return false;
    }
    // The PPA wants its output buffer address and size on cache lines (ppa_check_buffer_alignment)
    if (((uintptr_t)fb0 | (uintptr_t)fb1 | FB_BYTES) & (CONFIG_CACHE_L2_CACHE_LINE_SIZE - 1)) {
        ESP_LOGE(TAG, "direct flush: frame buffers %p %p not cache-line aligned", fb0, fb1);
        return false;
    }
    panel_fbs[0] = fb0;
    panel_fbs[1] = fb1;
    fb_done      = xSemaphoreCreateBinaryStatic(&fb_done_buf);
    return true;
}

// Replaces the port's DPI callbacks: its on_color_trans_done would call flush_ready from inside our draw_bitmap,
// and the swap needs the end-of-frame callback (on_vsync stays off, as before).
static bool direct_flush_callbacks(esp_lcd_panel_handle_t panel) {
    esp_lcd_dpi_panel_event_callbacks_t const cbs = {.on_frame_buf_complete = on_fb_done};
    esp_err_t const                           err = esp_lcd_dpi_panel_register_event_callbacks(panel, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "direct flush: frame callback: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
#endif

bool display_start(lv_display_t **out) {
    esp_ldo_channel_handle_t       ldo;
    esp_ldo_channel_config_t const ldo_cfg = {.chan_id = DSI_PHY_LDO_CHAN, .voltage_mv = DSI_PHY_LDO_MV};
    esp_err_t                      err     = esp_ldo_acquire_channel(&ldo_cfg, &ldo);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DSI PHY power: %s", esp_err_to_name(err));
        return false;
    }

    esp_lcd_dsi_bus_handle_t       bus;
    esp_lcd_dsi_bus_config_t const bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = DSI_LANES,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_MBPS,
    };
    esp_lcd_panel_io_handle_t     io;
    esp_lcd_dbi_io_config_t const dbi_cfg = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
    if ((err = esp_lcd_new_dsi_bus(&bus_cfg, &bus)) != ESP_OK ||
        (err = esp_lcd_new_panel_io_dbi(bus, &dbi_cfg, &io)) != ESP_OK) {
        ESP_LOGE(TAG, "DSI bus: %s", esp_err_to_name(err));
        return false;
    }

    esp_lcd_dpi_panel_config_t dpi_cfg       = ST7703_720_720_PANEL_60HZ_DPI_CONFIG();
    st7703_vendor_config_t     vendor_config = {
            .mipi_config    = {.dsi_bus = bus, .dpi_config = &dpi_cfg},
            .init_cmds      = init_cmds,
            .init_cmds_size = sizeof(init_cmds) / sizeof(init_cmds[0]),
    };
    esp_lcd_panel_dev_config_t const dev_cfg = {
        .bits_per_pixel          = 16,
        .rgb_ele_order           = LCD_RGB_ELEMENT_ORDER_RGB, // BGR swapped red and blue on the panel for weeks (#106): screenshots never showed it, they render from the LVGL buffer
        .reset_gpio_num          = LCD_IO_RST,
        .vendor_config           = &vendor_config,
        .flags.reset_active_high = 1,
    };
    esp_lcd_panel_handle_t panel;
    if ((err = esp_lcd_new_panel_st7703(io, &dev_cfg, &panel)) != ESP_OK ||
#if !CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH // the direct flush never calls draw_bitmap, so the DMA2D copy has no job
        (err = esp_lcd_dpi_panel_enable_dma2d(panel)) != ESP_OK ||
#endif
        (err = esp_lcd_panel_reset(panel)) != ESP_OK || (err = esp_lcd_panel_init(panel)) != ESP_OK) {
        ESP_LOGE(TAG, "panel: %s", esp_err_to_name(err));
        return false;
    }
    lcd_panel = panel;
    void *fb0 = NULL, *fb1 = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &fb0, &fb1) == ESP_OK) {
        // #86: who owns what in PSRAM
        ESP_LOGI(TAG, "panel framebuffers %p %p, %u KB each", fb0, fb1, (unsigned)(LCD_H_RES * LCD_V_RES * 2 >> 10));
    }

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity = 1;  // core 1 draws, core 0 talks to the world
    // The 7 KiB default left 1032 B at its high-water mark (#26). Internal RAM on purpose: the draw path
    // runs PPA/DMA2D and flash-backed font reads, which must not depend on a cached PSRAM stack.
    port_cfg.task_stack = LVGL_TASK_STACK;
    if ((err = lvgl_port_init(&port_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "LVGL: %s", esp_err_to_name(err));
        return false;
    }
#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH
    bool const direct = direct_flush_init(panel);
#endif
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = LCD_H_RES * DRAW_BUFFER_LINES,
        .double_buffer = true,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .flags.buff_spiram = 1,
        .flags.sw_rotate   = 1, // rotation requested; the port rotates on the CPU (LVGL_PORT_ENABLE_PPA=n since #95: the PPA pass through a second PSRAM buffer was slower)
    };
#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH
    if (direct) { // our flush turns the strips: no rotation buffer in the port, and no MADCTL writes
        disp_cfg.flags.sw_rotate = 0;
        disp_cfg.control_handle  = &no_hw_rotate;
    }
#endif
    lvgl_port_display_dsi_cfg_t const dsi_cfg = {.flags.avoid_tearing = 0};
    *out = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!*out) {
        ESP_LOGE(TAG, "LVGL display not added");
        return false;
    }
    // The panel is mounted on its side: the same turn BadgeVMS's compositor makes
    lvgl_port_lock(0);
#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH
    if (direct) {
        bool const swap_on = direct_flush_callbacks(panel);
        display_flip_init(&flip, &flip_ops, swap_on); // front = fbs[0], where dpi_panel_init starts the DMA
        display_engine_init(&engine, &engine_ops);
        // Every boot (#475: disp.engine is not saved); the first strip sets the client up, a failure falls back
        display_engine_select(&engine, DISPLAY_ENGINE_PPA_ASYNC);
        direct_disp = *out;
        lv_display_set_flush_cb(*out, flush_direct);
        lv_display_set_flush_wait_cb(*out, flush_wait);
        ESP_LOGI(TAG, "direct flush (%s), %s", display_engine_name(display_engine_active(&engine)),
                 swap_on ? "2 buffers" : "1 buffer");
    }
#endif
    lv_display_set_rotation(*out, LV_DISPLAY_ROTATION_90);
    lvgl_port_unlock();
    return true;
}

// disp.engine (#468). The caller holds the LVGL lock, so no refresh runs and nothing is in flight (the last strip of
// every refresh waits for its transfer): the switch applies at once, between two frames. Cache: no sync is needed at
// a switch. Every CPU write into a frame buffer (cpu_turn, flip_copy) is written back before it returns, so at any
// frame boundary no dirty line exists that a PPA invalidate could drop; a PPA transfer writes PSRAM after
// invalidating its window and is done before the CPU touches either buffer again, so no stale line survives the
// other way either.
// What stays open after a timeout: the timed-out transfer may still run. It can write its strip late, over the CPU's
// redo, from a draw buffer LVGL may have rendered anew since (one wrong strip until that area is redrawn). It cannot
// pair with a new transfer: its done is ignored (tags), and no PPA engine is selected again until that done is seen.
bool display_engine_get(uint32_t *id) {
#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH
    if (direct_disp) {
        *id = display_engine_active(&engine);
        return true;
    }
#endif
    (void)id;
    return false;
}

bool display_engine_set(uint32_t id) {
#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH && CONFIG_LICHEN_DEV_COMMANDS
    if (!direct_disp || !display_engine_valid(id)) {
        return false;
    }
    display_engine_ppa_done_seen(&engine, ppa_gen_last_done);
    if (!display_engine_select(&engine, id)) {
        if (display_engine_ppa_hung(&engine)) {
            ESP_LOGW(TAG, "rotate: the PPA has not finished the transfer that timed out, staying on cpu");
        }
        return false;
    }
    ESP_LOGI(TAG, "rotate: %s", display_engine_name(display_engine_active(&engine)));
    return true;
#else
    (void)id;
    return false;
#endif
}

#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH && CONFIG_LICHEN_DEV_COMMANDS
// #558 spike: one scale + rotate frame of the planned video path into the back buffer, timed. The UI rectangle 720x537
// centred in 720x720 (rows 91..627); the PPA's output block comes out 537 wide by 720 tall (ppa_srm.c new_block_w/h).
// Same hand-over as a background strip (tag, done semaphore) but waited for here, so the engine is idle again at return.
vbench_status_t display_vbench_frame(void const *src, vbench_frame_t *out) {
    if (!direct_disp || !ppa_up()) { // on the LVGL task: the DMA2D pool's interrupts go to its core
        return VBENCH_NO_PPA;
    }
    display_engine_ppa_done_seen(&engine, ppa_gen_last_done);
    display_engine_settle(&engine);
    if (display_engine_ppa_hung(&engine)) {
        return VBENCH_NO_PPA;
    }
    unsigned const       fb  = display_flip_write_target(&flip); // never front ^ 1: a pending swap makes that the shown one
    display_rect_t const ui  = {0, 91, LCD_H_RES - 1, 91 + 537 - 1};
    display_rect_t const blk = display_rot90_block(ui, LCD_V_RES);
    uint32_t const       gen = engine.gen = display_engine_gen_next(engine.gen);
    ppa_srm_oper_config_t const op = {
        .in.buffer          = src,
        .in.pic_w           = VBENCH_SRC_W,
        .in.pic_h           = VBENCH_SRC_H,
        .in.block_w         = VBENCH_SRC_W,
        .in.block_h         = VBENCH_SRC_H,
        .in.srm_cm          = PPA_SRM_COLOR_MODE_RGB565,
        .out.buffer         = panel_fbs[fb],
        .out.buffer_size    = FB_BYTES,
        .out.pic_w          = LCD_H_RES,
        .out.pic_h          = LCD_V_RES,
        .out.block_offset_x = blk.x1,
        .out.block_offset_y = blk.y1,
        .out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
        .rotation_angle     = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x            = VBENCH_SCALE_X,
        .scale_y            = VBENCH_SCALE_Y,
        .mode               = PPA_TRANS_MODE_NON_BLOCKING,
        .user_data          = (void *)(uintptr_t)gen,
    };
    xSemaphoreTake(ppa_done, 0);
    ppa_ready_in_isr = false;
    ppa_gen_current  = gen;
    uint32_t const t0 = (uint32_t)esp_timer_get_time();
    if (ppa_do_scale_rotate_mirror(ppa_srm, &op) != ESP_OK) {
        ppa_gen_current = 0;
        return VBENCH_REFUSED;
    }
    out->call_us = (uint32_t)esp_timer_get_time() - t0;
    uint32_t done_us = 0;
    if (!ppa_wait(NULL, &done_us, false)) {
        bool const late = ppa_wait(NULL, &done_us, true);
        if (!late) {
            engine.hung     = true; // as late_outcome: until its own done turns up
            engine.hung_gen = gen;
        }
        display_engine_select(&engine, DISPLAY_ENGINE_CPU);
        return VBENCH_TIMEOUT;
    }
    out->total_us = done_us - t0;
    out->fb       = panel_fbs[fb];
    out->x1       = blk.x1;
    out->x2       = blk.x2;
    return VBENCH_OK;
}
#endif

void display_blank(void) {
    if (lcd_panel) {
        esp_lcd_panel_disp_on_off(lcd_panel, false);
    }
}

// Registry reads (#317, domain 0x12). The pixel clock is read back from the divider and source select the DPI driver
// programmed, over the clock tree's source frequency, not taken from the 36 in the panel config (the divider rounds).
// No bus traffic, no lock; false before display_start() succeeded.
bool display_dpi_hz(uint32_t *hz) {
    if (!lcd_panel) {
        return false;
    }
    soc_module_clk_t src;
    switch (HP_SYS_CLKRST.peri_clk_ctrl03.reg_mipi_dsi_dpiclk_src_sel) {
    case 0:  src = SOC_MOD_CLK_XTAL; break;
    case 1:  src = SOC_MOD_CLK_PLL_F240M; break;
    case 2:  src = SOC_MOD_CLK_PLL_F160M; break;
    default: return false; // APLL: not a source this firmware uses, and its rate is not fixed
    }
    uint32_t src_hz = 0;
    if (esp_clk_tree_src_get_freq_hz(src, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &src_hz) != ESP_OK) {
        return false;
    }
    *hz = disp_pclk_hz(src_hz, HP_SYS_CLKRST.peri_clk_ctrl03.reg_mipi_dsi_dpiclk_div_num);
    return true;
}

// Refresh in 0.1 Hz from the applied pixel clock and the totals handed to the panel (st7703_bono.h)
bool display_refresh_dhz(uint32_t *dhz) {
    uint32_t hz;
    if (!display_dpi_hz(&hz)) {
        return false;
    }
    *dhz = disp_refresh_dhz(hz, ST7703_HTOTAL, ST7703_VTOTAL);
    return true;
}

// The IDF has no read-back for these three (no lane-count or PHY PLL getter, no LDO voltage getter): configured values
bool display_dsi_lanes(uint32_t *n) {
    *n = DSI_LANES;
    return lcd_panel != NULL;
}

bool display_dsi_lane_mbps(uint32_t *mbps) {
    *mbps = DSI_LANE_MBPS;
    return lcd_panel != NULL;
}

bool display_phy_ldo_mv(uint32_t *mv) {
    *mv = DSI_PHY_LDO_MV;
    return lcd_panel != NULL;
}
