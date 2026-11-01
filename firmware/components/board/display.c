#include "display.h"

#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7703.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "st7703_bono.h"

#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH
#include "display_rot.h"
#include "esp_lcd_panel_interface.h"
#include <stdint.h>
#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH_PPA
#include "driver/ppa.h"
#else
#include "esp_cache.h"
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
// Tearing is no worse than before: there is one frame buffer, scanned out while it is written, and the
// DMA2D copy wrote into it the same way.
#define FB_BYTES (LCD_H_RES * LCD_V_RES * 2)
#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH_PPA
#define DIRECT_FLUSH_ENGINE "PPA"
#else
#define DIRECT_FLUSH_ENGINE "CPU"
#endif

static uint16_t *panel_fb;
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

static void flush_direct(lv_display_t *disp, lv_area_t const *area, uint8_t *px_map) {
    display_rect_t const ui  = {area->x1, area->y1, area->x2, area->y2};
    display_rect_t const blk = display_rot90_block(ui, LCD_V_RES); // UI width = panel height
    int32_t const        w   = ui.x2 - ui.x1 + 1;
    int32_t const        h   = ui.y2 - ui.y1 + 1;
    uint32_t const       stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH_PPA
    // Cache: ppa_do_scale_rotate_mirror writes the strip back (C2M, unaligned allowed) before the PPA reads it,
    // and invalidates the destination rows (M2C) itself, so there is no msync here. The CPU never writes the
    // frame buffer in this variant, so that invalidate drops nothing dirty.
    // TODO(#95 e2): PPA_TRANS_MODE_NON_BLOCKING with on_trans_done -> lv_display_flush_ready, so LVGL renders
    // the next strip while the PPA turns this one. The callback runs in the DMA2D ISR (ppa_transaction_done_cb),
    // the same context the port's DPI done callback calls flush_ready from today; measure e1 first.
    ppa_srm_oper_config_t const op = {
        .in.buffer          = px_map,
        .in.pic_w           = stride / 2,
        .in.pic_h           = h,
        .in.block_w         = w,
        .in.block_h         = h,
        .in.srm_cm          = PPA_SRM_COLOR_MODE_RGB565,
        .out.buffer         = panel_fb,
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
    uint16_t *const dst_row = panel_fb + blk.y1 * LCD_H_RES;
    lv_draw_sw_rotate(px_map, dst_row + blk.x1, w, h, stride, LCD_H_RES * 2, LV_DISPLAY_ROTATION_90, LV_COLOR_FORMAT_RGB565);
    esp_cache_msync(dst_row, (size_t)(blk.y2 - blk.y1 + 1) * LCD_H_RES * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
#endif
    lv_display_flush_ready(disp);
}

// Frame buffer and (for the PPA) the SRM client. False = fall back to the port's path (with a CPU copy
// instead of DMA2D, which is not enabled in this configuration).
static bool direct_flush_init(esp_lcd_panel_handle_t panel) {
    void *fb = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb) != ESP_OK || !fb) {
        ESP_LOGE(TAG, "direct flush: no frame buffer");
        return false;
    }
    // The PPA wants its output buffer address and size on cache lines (ppa_check_buffer_alignment)
    if (((uintptr_t)fb | FB_BYTES) & (CONFIG_CACHE_L2_CACHE_LINE_SIZE - 1)) {
        ESP_LOGE(TAG, "direct flush: frame buffer %p not cache-line aligned", fb);
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
    panel_fb = fb;
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
    void *fb = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb) == ESP_OK) {
        ESP_LOGW(TAG, "panel framebuffer %p, %u KB", fb, (unsigned)(LCD_H_RES * LCD_V_RES * 2 >> 10)); // #86: who owns what in PSRAM
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
        lv_display_set_flush_cb(*out, flush_direct);
        ESP_LOGW(TAG, "direct flush (%s) into the frame buffer", DIRECT_FLUSH_ENGINE);
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
