// The display fingerprint (#354): the line of disp_state_fmt.c logged once per boot, after the 120 MHz boot has settled
// (60 s), and on demand with `fw disp state`. Only reads existing getters (registry cache, PSRAM speed record, C6
// watch, saved backlight levels); no state of its own and no NVS write.

#include "c6link.h"
#include "disp_state_fmt.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "lichen_os.h"
#include "lichen_sys_display.h"
#include "psram_speed.h"

#include <stdio.h>
#include <string.h>

static char const TAG[] = "disp";

// After PSRAM_SPEED_SETTLE_SECONDS (60) plus a margin, so arms is cleared and a first heap check is in
#define LOG_AT_US (65LL * 1000000)

#define KEY_LEN 48

static void gather(ds_info_t *in, char bufs[][KEY_LEN]) {
    static char const *const keys[] = {"psram.clk", "disp.pclk", "disp.refresh", "bl.display", "bl.keyboard",
                                       "bl.boot", "bl.idle", "bl.override", "bl.freq", "psram.fail_why"};
    char const **raw[] = {&in->psram_clk_raw, &in->pclk_raw, &in->refresh_raw, &in->bl_raw, &in->kbd_raw,
                          &in->bl_boot_raw, &in->bl_idle_raw, &in->bl_override_raw, &in->bl_freq_raw, &in->fall_raw};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        bufs[i][0] = '\0';
        *raw[i]    = lichen_reg_cached_text(keys[i], bufs[i], KEY_LEN) ? bufs[i] : NULL;
    }
    psram_speed_rec_t rec;
    psram_speed_record(&rec);
    in->psram_want_mhz = (int)psram_speed_want();
    in->failed         = rec.failed ? 1 : 0;
    in->arms           = rec.arms;
    in->heap_max_us    = psram_speed_heap_max_us() ? (int64_t)psram_speed_heap_max_us() : -1;
    uint8_t d = 0, k = 0;
    lichen_sys_display_get(&d, &k);
    in->bl_req  = d;
    in->kbd_req = k;
    c6_status_t c6;
    c6link_status(&c6);
    in->c6_up       = c6link_up() ? 1 : 0;
    in->c6_restarts = c6.restarts;
    in->uptime_s    = esp_timer_get_time() / 1000000;
}

static size_t line(char *out, size_t cap) {
    char      bufs[10][KEY_LEN];
    ds_info_t in = {0};
    gather(&in, bufs);
    in.reset = psram_speed_reset_name((uint8_t)esp_reset_reason());
    in.fw    = esp_app_get_description()->version;
    return ds_format(&in, out, cap);
}

static void log_once(void *arg) {
    (void)arg;
    char text[320];
    line(text, sizeof(text));
    ESP_LOGI(TAG, "%s", text);
}

static void cmd_disp(char const *args) {
    if (strcmp(args, "state") != 0 && args[0] != '\0') {
        console_reply("disp: usage: disp state (the display fingerprint line, #354)");
        return;
    }
    char text[320];
    line(text, sizeof(text));
    console_reply("%s", text);
}

void disp_state_start(void) {
    console_register("disp", "disp [state] (the display fingerprint line, logged once 65 s after boot, #354)", cmd_disp);
    static esp_timer_handle_t     timer;
    esp_timer_create_args_t const args = {.callback = log_once, .name = "disp_state"};
    int64_t                       wait = LOG_AT_US - esp_timer_get_time();
    if (esp_timer_create(&args, &timer) != ESP_OK ||
        esp_timer_start_once(timer, wait > 1000000 ? (uint64_t)wait : 1000000) != ESP_OK) {
        ESP_LOGW(TAG, "no timer: the fingerprint line is only printed by `fw disp state`");
    }
}
