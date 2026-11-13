// "fw soak" (#409): reads the values soak_line.c formats. Main is the one place that may see every component.
// Rules for each read: no allocation, no flash or file I/O, no lock held across a reply. A value another task owns
// comes through that task's getter (a copy under its spinlock, an atomic, or a cache the owner refreshes); a value
// that cannot be read now stays SOAK_NA. The one wait is MeshCore's lock, bounded by MESH_WAIT_MS.
// Cost of one read: three heap walks for the largest blocks (heap_caps_get_largest_free_block, as fw mem), the
// mesh lock for a few loads, and short spinlock copies; no NVS, no /storage, no SD, no message to the C6.
#include "soak_cmd.h"

#ifdef CONFIG_LICHEN_DEV_COMMANDS
#include "soak_line.h"

#include "backlight.h"
#include "battery.h"
#include "c6_watch.h"
#include "c6link.h"
#include "crash.h"
#include "dma_watch.h"
#include "esp_attr.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "keyboard.h"
#include "lichen_os.h"
#include "lichen_sys_thermal.h"
#include "lichen_sys_ui.h"
#include "lora_os.h"
#include "mem_window_watch.h"
#include "mesh.h"
#include "ota_trial.h"
#include "psram_guard.h"
#include "sdcard.h"
#include "storage.h"
#include "wifi_service.h"

#define MESH_WAIT_MS 50 // MeshCore's loop holds its lock briefly; a sample taken while it is busy prints -1

// soak_line.c's word tables follow these numbers
_Static_assert(ESP_RST_UNKNOWN == 0 && ESP_RST_SW == 3 && ESP_RST_WDT == 7 && ESP_RST_CPU_LOCKUP == 15,
               "esp_reset_reason_t changed: update soak_line.c RESET_W");
_Static_assert(C6W_UP == 0 && C6W_DOWN == 1 && C6W_ATTACHING == 2 && C6W_GAVE_UP == 3, "update soak_line.c C6_W");
_Static_assert(TH_OK == 0 && TH_WARN == 1 && TH_SHUTDOWN == 2, "update soak_line.c TH_W");
_Static_assert(SD_STATE_OFF == 0 && SD_STATE_NO_CARD == 1 && SD_STATE_NOT_FAT == 2 && SD_STATE_ERROR == 3 &&
                   SD_STATE_MOUNTED == 4 && SD_STATE_REMOVED == 5,
               "update soak_line.c SD_W");
_Static_assert(BAT_GAUGE_UNPROBED == 0 && BAT_GAUGE_ABSENT == 1 && BAT_GAUGE_LINES_LOW == 2 && BAT_GAUGE_NO_BUS == 3 &&
                   BAT_GAUGE_PRESENT == 4 && BAT_GAUGE_STALE == 5 && BAT_GAUGE_LOST == 6,
               "update soak_line.c BAT_W");

// One command runs at a time (console.c's run mutex), so one copy serves the UART and the Wi-Fi console; in PSRAM's
// no-init segment (soak_init fills it on every read), off the 4 KB console stack and out of internal RAM
EXT_RAM_NOINIT_ATTR static soak_t s_snap;
EXT_RAM_NOINIT_ATTR static char   s_line[SOAK_LINE_MAX];

static void read_boot(soak_t *s) {
    uint32_t n;
    if (boot_count(&n)) {
        s->boot = n;
    }
    s->up_ms       = esp_timer_get_time() / 1000;
    s->reset       = esp_reset_reason();
    s->reset_crash = crash_recovered() ? 1 : 0;
    if (crash_count_cached(&n)) {
        s->crashes = n;
    }
    esp_partition_t const *run = esp_ota_get_running_partition(); // cached by the IDF after the first call
    if (run && run->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 && run->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_1) {
        s->slot = run->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
    }
    ota_image_state_t trial = ota_image_trial_known(); // never reads the OTA data
    if (trial != OTA_IMAGE_UNKNOWN) {
        s->slot_ok = trial == OTA_IMAGE_NOT_ON_TRIAL ? 1 : 0;
    }
}

static void read_memory(soak_t *s) {
    mem_heap_fig_t h[MEM_WINDOW_HEAPS], lp;
    bool           have_lp = mem_window_watch_heaps(h, &lp);
    int64_t       *fields[MEM_WINDOW_HEAPS][3] = {{&s->int_free, &s->int_min, &s->int_big},
                                                  {&s->dma_free, &s->dma_min, &s->dma_big},
                                                  {&s->ps_free, &s->ps_min, &s->ps_big}};
    for (int i = 0; i < MEM_WINDOW_HEAPS; i++) {
        *fields[i][0] = h[i].free;
        *fields[i][1] = h[i].min;
        *fields[i][2] = h[i].big;
    }
    if (have_lp) {
        s->lp_free = lp.free;
        s->lp_min  = lp.min;
    }
    uint32_t min[MEM_WINDOW_HEAPS];
    bool     exact[MEM_WINDOW_HEAPS];
    if (mem_window_watch_read(min, exact)) {
        s->int_win = min[MEM_WINDOW_INTERNAL];
        s->int_wx  = exact[MEM_WINDOW_INTERNAL];
        s->dma_win = min[MEM_WINDOW_DMA];
        s->dma_wx  = exact[MEM_WINDOW_DMA];
        s->ps_win  = min[MEM_WINDOW_PSRAM];
        s->ps_wx   = exact[MEM_WINDOW_PSRAM];
    }
    uint32_t drops;
    if (dma_watch_drops(&drops)) {
        s->dma_drops = drops;
    }
}

static void read_c6(soak_t *s) {
    s->lock_hits = lichen_sys_ui_lockcheck_hits();
    s->canary    = psram_guard_hits();
    c6_status_t st; // ~190 B on the stack: c6link_status copies under a spinlock, so not into PSRAM
    c6link_status(&st);
    s->c6_state    = st.state;
    s->c6_restarts = st.restarts;
    uint32_t beats, misses, dropped;
    c6link_hb_counts(&beats, &misses, &dropped);
    s->hb_beats   = beats;
    s->hb_misses  = misses;
    s->hb_dropped = dropped;
    uint16_t in_use, peak;
    uint32_t waits, drops;
    if (c6link_frame_pool(true, &in_use, &peak, &waits, &drops)) {
        s->pool_tx_drops = drops;
    }
    if (c6link_frame_pool(false, &in_use, &peak, &waits, &drops)) {
        s->pool_rx_drops = drops;
    }
}

static void read_lora(soak_t *s) {
    lora_c6_counts_t c;
    if (lora_get_c6_counts(&c)) {
        s->lora_state   = c.state;
        s->lora_packets = c.packets;
        s->lora_crc     = c.crc_errors;
        s->c6_up_s      = c.c6_uptime_s;
        s->lora_age_s   = c.age_s;
    }
    uint32_t rx, tx;
    lora_get_counts(&rx, &tx);
    s->lora_rx = rx;
    s->lora_tx = tx;
}

static void read_mesh(soak_t *s) {
    mesh_soak_counts_t m;
    if (mesh_soak_counts(&m, MESH_WAIT_MS)) {
        s->mesh_rx         = m.rx;
        s->mesh_rx_dropped = m.rx_dropped;
        s->mesh_tx         = m.tx;
        s->mesh_failed     = m.failed;
        s->mesh_timeouts   = m.timeouts;
        s->mesh_refused    = m.refused;
    }
    uint32_t      seq;
    airlog_info_t info;
    mesh_airlog_snapshot(NULL, 0, &seq, &info); // the counts only: its own short lock, never MeshCore's
    s->air_logged  = seq;
    s->air_dropped = info.dropped;
}

static void read_net_and_heat(soak_t *s) {
    int8_t rssi;
    s->wifi_dbm = wifi_rssi(&rssi) ? rssi : 0;
    s->wifi_up  = wifi_has_ip(NULL, 0) ? 1 : 0; // the address itself is never copied
    lichen_sys_thermal_info_t th;
    lichen_sys_thermal_info(&th);
    s->thermal = th.level;
}

static void read_storage(soak_t *s) {
    uint64_t free_b, total_b;
    if (storage_free_bytes_cached(&free_b, &total_b)) { // the cache: the live count walks the drive under its lock
        s->storage_free = (int64_t)free_b;
    }
    sd_info_t sd;
    sd_info(&sd);
    s->sd_state  = sd.state;
    s->sd_ok     = sd.state == SD_STATE_MOUNTED ? 1 : 0;
    s->sd_errors = sd.io_errors;
    if (sd.state == SD_STATE_MOUNTED && sd.free_known) {
        s->sd_free = (int64_t)sd.free_bytes;
    }
}

static void read_power_and_keys(soak_t *s) {
    battery_snap_t bat;
    battery_get(&bat);
    s->battery = bat.gauge;
    if (bat.have) {
        s->battery_mv = bat.mv;
    }
    uint32_t resets;
    s->kbd_ok     = keyboard_chip_state(&resets);
    s->kbd_resets = resets;
    uint8_t display, keys;
    backlight_get_requested(&display, &keys);
    s->bl_req  = display;
    s->kbl_req = keys;
    uint32_t applied;
    if (lichen_reg_cached_u32("bl.display", &applied)) {
        s->bl_app = applied;
    }
    if (lichen_reg_cached_u32("bl.keyboard", &applied)) {
        s->kbl_app = applied;
    }
}

static void cmd_soak(char const *args) {
    if (*args) {
        console_reply("soak (one snapshot in key=value lines soak1..soak%d, numbers and words only; -1 or na: not "
                      "readable now)",
                      SOAK_LINES);
        return;
    }
    soak_t *s = &s_snap;
    soak_init(s);
    read_boot(s);
    read_memory(s);
    read_c6(s);
    read_lora(s);
    read_mesh(s);
    read_net_and_heat(s);
    read_storage(s);
    read_power_and_keys(s);
    for (unsigned n = 1; n <= SOAK_LINES; n++) {
        if (soak_line(s, n, s_line, sizeof(s_line))) {
            console_reply("%s", s_line);
        }
    }
}

void soak_cmd_register(void) {
    console_register_dev("soak", "soak (the badge's state in key=value lines for an unattended sampler, #409; no names "
                                 "or addresses)",
                         cmd_soak);
}
#endif
