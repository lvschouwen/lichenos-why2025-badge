#include "display.h"

#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7703.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "st7703_bono.h"

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
        .rgb_ele_order           = LCD_RGB_ELEMENT_ORDER_BGR, // the badge's panel is wired backwards
        .reset_gpio_num          = LCD_IO_RST,
        .vendor_config           = &vendor_config,
        .flags.reset_active_high = 1,
    };
    esp_lcd_panel_handle_t panel;
    if ((err = esp_lcd_new_panel_st7703(io, &dev_cfg, &panel)) != ESP_OK ||
        (err = esp_lcd_dpi_panel_enable_dma2d(panel)) != ESP_OK || (err = esp_lcd_panel_reset(panel)) != ESP_OK ||
        (err = esp_lcd_panel_init(panel)) != ESP_OK) {
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
    lvgl_port_display_cfg_t const disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = LCD_H_RES * DRAW_BUFFER_LINES,
        .double_buffer = true,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .flags.buff_spiram = 1,
        .flags.sw_rotate   = 1, // rotation requested; with LVGL_PORT_ENABLE_PPA=y the PPA does it
    };
    lvgl_port_display_dsi_cfg_t const dsi_cfg = {.flags.avoid_tearing = 0};
    *out = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!*out) {
        ESP_LOGE(TAG, "LVGL display not added");
        return false;
    }
    // The panel is mounted on its side: the same turn BadgeVMS's compositor makes
    lvgl_port_lock(0);
    lv_display_set_rotation(*out, LV_DISPLAY_ROTATION_90);
    lvgl_port_unlock();
    return true;
}

void display_blank(void) {
    if (lcd_panel) {
        esp_lcd_panel_disp_on_off(lcd_panel, false);
    }
}
