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
#include "display_flip.h"
#include "display_rot.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_lcd_panel_interface.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdint.h>
#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH_PPA
#include "driver/ppa.h"
#endif
#endif

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
// DMA is not reading; after the last strip the buffers swap, and the areas that frame drew are copied into the
// new back buffer so both hold the same picture again (PARTIAL rendering only redraws what changed).
// How the IDF 6.1 DPI driver swaps (esp_lcd/dsi/esp_lcd_panel_dpi.c, read, not changed):
// - draw_bitmap with a pointer inside fbs[i] copies nothing: it writes back the cache of the rows it names, sets
//   cur_fb_index = i and calls on_color_trans_done at once (dpi_panel_draw_bitmap_2d, lines 613-637).
// - The DMA reads one whole frame buffer per transfer; its done interrupt (mipi_dsi_dma_trans_done_cb, lines
//   83-103) reads cur_fb_index, restarts on that buffer's link list and then calls on_frame_buf_complete. So the
//   switch takes effect at the next end of frame, and the first on_frame_buf_complete after the index was set
//   says the old buffer is no longer read. That ISR is not IRAM (DW_GDMA_ISR_IRAM_SAFE off), so neither is ours.
// - The interrupt reads the index a few instructions before the callback, so a callback stamped just after the
//   request may have restarted on the old buffer: display_swap_proven() waits one more frame then.
// The driver starts on fbs[0] (dpi_panel_init) and callocs both, so both start equal (black).
#define FB_BYTES        (LCD_H_RES * LCD_V_RES * 2)
#define SWAP_GUARD_US   200 // the ISR's index read to callback stamp is well under this
#define SWAP_TIMEOUT_MS 100 // about two frames at 47 Hz plus slack: a missed end of frame costs this, no more
#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH_PPA
#define DIRECT_FLUSH_ENGINE "PPA"
#else
#define DIRECT_FLUSH_ENGINE "CPU"
#endif

static uint16_t         *panel_fbs[2];
static unsigned          fb_front;    // the buffer the DMA scans out (LVGL task only)
EXT_RAM_BSS_ATTR static display_dirty_t frame_dirty; // panel areas this frame wrote into the back buffer (LVGL task only)
static SemaphoreHandle_t fb_done;     // given by on_fb_done at every end of frame
static StaticSemaphore_t fb_done_buf;
static uint32_t volatile fb_done_us; // low 32 bits of esp_timer at the last end of frame (32 bits: one store)
static uint32_t          swap_misses;
static bool              swap_on; // the end-of-frame callback is ours; false = no swap, each frame copied to the shown buffer
#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH_PPA
static ppa_client_handle_t ppa_srm; // kept for the display's lifetime
#endif

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

// DMA done ISR, once per scanned frame: kept to a stamp and a give
static bool on_fb_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *ctx) {
    (void)panel;
    (void)edata;
    (void)ctx;
    fb_done_us       = (uint32_t)esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(fb_done, &woken);
    return woken == pdTRUE;
}

// Wait until an end of frame proves the DMA restarted after `after_us`. Bounded: a missed end of frame (scan-out
// stalled by a flash erase, panel off) costs SWAP_TIMEOUT_MS and a log line, never a hung UI.
static bool wait_swap(uint32_t after_us) {
    TickType_t const until = xTaskGetTickCount() + pdMS_TO_TICKS(SWAP_TIMEOUT_MS);
    for (;;) {
        TickType_t const left = until - xTaskGetTickCount();
        if ((int32_t)left <= 0 || xSemaphoreTake(fb_done, left) != pdTRUE) {
            return false;
        }
        if (display_swap_proven(after_us, fb_done_us, SWAP_GUARD_US)) {
            return true;
        }
    }
}

// Show the back buffer at the next end of frame. False when there is no swap (the picture then reaches the front
// buffer by the copy-back alone, as torn as before #467).
static bool show_back(void) {
    if (!swap_on) {
        return false;
    }
    unsigned const back = display_back_fb(fb_front);
    xSemaphoreTake(fb_done, 0); // an end of frame from before the request proves nothing
    // Row 0 is the cache write-back the driver does for a 1x1 area; every strip was written back already
    esp_err_t const err = esp_lcd_panel_draw_bitmap(lcd_panel, 0, 0, 1, 1, panel_fbs[back]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "frame buffer swap: %s", esp_err_to_name(err));
        return false;
    }
    if (!wait_swap((uint32_t)esp_timer_get_time()) && (swap_misses++ % 100) == 0) {
        ESP_LOGW(TAG, "swap: no end of frame in %d ms (%u)", SWAP_TIMEOUT_MS, (unsigned)swap_misses);
    }
    fb_front = back; // the index is set: the DMA switches whenever it next restarts
    return true;
}

// End of an LVGL refresh: swap, then copy what this frame drew into the buffer that is now the back one. The copy is
// written back to PSRAM at once: the PPA variant invalidates its destination rows before it writes, which would drop
// dirty lines of this copy.
static void finish_frame(void) {
    unsigned const drawn = display_back_fb(fb_front);
    show_back();
    uint16_t *const       dst = panel_fbs[display_back_fb(drawn)];
    uint16_t const *const src = panel_fbs[drawn];
    for (uint32_t i = 0; i < frame_dirty.n; i++) {
        display_rect_t const r = frame_dirty.r[i];
        display_copy_rect(dst, src, LCD_H_RES, r);
        esp_cache_msync(dst + r.y1 * LCD_H_RES, (size_t)(r.y2 - r.y1 + 1) * LCD_H_RES * 2,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
    display_dirty_clear(&frame_dirty);
}

static void flush_direct(lv_display_t *disp, lv_area_t const *area, uint8_t *px_map) {
    display_rect_t const ui  = {area->x1, area->y1, area->x2, area->y2};
    display_rect_t const blk = display_rot90_block(ui, LCD_V_RES); // UI width = panel height
    int32_t const        w   = ui.x2 - ui.x1 + 1;
    int32_t const        h   = ui.y2 - ui.y1 + 1;
    uint32_t const       stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
    uint16_t *const      fb     = panel_fbs[display_back_fb(fb_front)];
#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH_PPA
    // Cache: ppa_do_scale_rotate_mirror writes the strip back (C2M, unaligned allowed) before the PPA reads it,
    // and invalidates the destination rows (M2C) itself, so there is no msync here. The only CPU writes into a
    // frame buffer are finish_frame's copies, which are written back before the next strip.
    ppa_srm_oper_config_t const op = {
        .in.buffer          = px_map,
        .in.pic_w           = stride / 2,
        .in.pic_h           = h,
        .in.block_w         = w,
        .in.block_h         = h,
        .in.srm_cm          = PPA_SRM_COLOR_MODE_RGB565,
        .out.buffer         = fb,
        .out.buffer_size    = FB_BYTES,
        .out.pic_w          = LCD_H_RES,
        .out.pic_h          = LCD_V_RES,
        .out.block_offset_x = blk.x1,
        .out.block_offset_y = blk.y1,
        .out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
        .rotation_angle     = PPA_SRM_ROTATION_ANGLE_90, // counter-clockwise, as LV_DISPLAY_ROTATION_90 (the port maps them 1:1)
        .scale_x            = 1.0f,
        .scale_y            = 1.0f,
        .mode               = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t const err = ppa_do_scale_rotate_mirror(ppa_srm, &op);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PPA flush %d,%d..%d,%d: %s", (int)ui.x1, (int)ui.y1, (int)ui.x2, (int)ui.y2, esp_err_to_name(err));
    }
#else
    // The turned strip is LCD_V_RES rows of h pixels: write back the whole row span in one msync, as the DPI
    // driver's own CPU copy path does (only dirty lines cost a PSRAM write).
    uint16_t *const dst_row = fb + blk.y1 * LCD_H_RES;
    lv_draw_sw_rotate(px_map, dst_row + blk.x1, w, h, stride, LCD_H_RES * 2, LV_DISPLAY_ROTATION_90, LV_COLOR_FORMAT_RGB565);
    esp_cache_msync(dst_row, (size_t)(blk.y2 - blk.y1 + 1) * LCD_H_RES * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
#endif
    display_dirty_add(&frame_dirty, blk);
    if (lv_display_flush_is_last(disp)) {
        finish_frame();
    }
    lv_display_flush_ready(disp);
}

// Frame buffers and (for the PPA) the SRM client. False = fall back to the port's path (with a CPU copy
// instead of DMA2D, which is not enabled in this configuration); it draws into fbs[0], the one scanned out.
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
#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH_PPA
    ppa_client_config_t const cfg = {.oper_type = PPA_OPERATION_SRM};
    esp_err_t const           err = ppa_register_client(&cfg, &ppa_srm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "direct flush: PPA client: %s", esp_err_to_name(err));
        return false;
    }
#endif
    panel_fbs[0] = fb0;
    panel_fbs[1] = fb1;
    fb_front     = 0; // dpi_panel_init starts the DMA on fbs[0]
    display_dirty_clear(&frame_dirty);
    fb_done = xSemaphoreCreateBinaryStatic(&fb_done_buf);
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
        swap_on = direct_flush_callbacks(panel);
        lv_display_set_flush_cb(*out, flush_direct);
        ESP_LOGI(TAG, "direct flush (%s), %s", DIRECT_FLUSH_ENGINE, swap_on ? "2 buffers" : "1 buffer");
    }
#endif
    lv_display_set_rotation(*out, LV_DISPLAY_ROTATION_90);
    lvgl_port_unlock();
    return true;
}

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
