// The SD card (#60): power, probe, FAT mount and the batch mutex. The pins, LDO channel and slot are checked against
// IDF 6.1 (esp_hal_sd/esp32p4/include/soc/sdmmc_pins.h: slot 0 IO-MUX = CLK 43, CMD 44, D0-D3 39-42) and the
// hardware notes on issue #60 (BadgeVMS drivers/fatfs.c, the M.2 card's schematic: VO4 = V/SDMMC).
#include "sdcard.h"

#include "board_sdmmc.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_protocol_defs.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char const TAG[] = "sd";

#define SD_LDO_CHAN     4    // LDO_VO4: VDD_IO_5 (GPIO39-48) and the card's VDD
#define SD_POWER_UP_MS  10   // card VDD ramp before the first command
#define SD_BOUNCE_BYTES 4096 // our own bounce buffer in PSRAM, so unaligned sector I/O never allocates internal DMA RAM
#define SD_MAX_FILES    4
#define SD_MKFS_WORK    (32 * 1024)
#define SD_LOCK_WAIT_MS 10000 // mount, unmount and format wait this long for a running batch
#define SD_HOST_WAIT_MS 10000 // and this long for the C6 link to finish adding or removing its slot

static SemaphoreHandle_t lock;
static StaticSemaphore_t lock_buf;
static portMUX_TYPE      info_lock = portMUX_INITIALIZER_UNLOCKED;
static sd_info_t         info;
static atomic_bool       mounted;
static sd_io_count_t     io;

// What is up right now, so bring_down() undoes exactly that. Only touched with `lock` held.
static sd_pwr_ctrl_handle_t pwr;
static void                *bounce;
static bool                 slot_up;
static sdmmc_card_t        *card;
static BYTE                 pdrv = 0xFF;
static char                 drv[3];
static FATFS               *fs;
static bool                 vfs_up;

static void set_reason(char *reason, size_t cap, char const *what, esp_err_t err) {
    snprintf(reason, cap, "%s: %s", what, esp_err_to_name(err));
}

static void publish_card(void) {
    uint64_t bytes = (uint64_t)card->csd.capacity * (uint64_t)card->csd.sector_size;
    portENTER_CRITICAL(&info_lock);
    strlcpy(info.kind, sd_card_kind(card->is_mmc, (card->ocr & SD_OCR_SDHC_CAP) != 0, bytes), sizeof(info.kind));
    info.total_bytes = bytes; // the card's size until the FAT volume's is known
    info.freq_khz    = card->real_freq_khz;
    info.width       = (uint8_t)(1u << card->log_bus_width);
    portEXIT_CRITICAL(&info_lock);
}

// The FAT volume's size and free space; f_getfree reads FSINFO or its cached count, so this is cheap after the first
// call on a card with a valid FSINFO
static void refresh_space(void) {
    uint64_t total, avail;
    bool     ok = esp_vfs_fat_info(SD_BASE, &total, &avail) == ESP_OK;
    portENTER_CRITICAL(&info_lock);
    if (ok) {
        info.total_bytes = total;
        info.free_bytes  = avail;
    }
    info.free_known = ok;
    portEXIT_CRITICAL(&info_lock);
}

static void bring_down(void) {
    atomic_store(&mounted, false);
    if (vfs_up) {
        // Always, mounted or not: f_mount(fs, drv, 1) registers fs in FatFs's volume table before it even tries, so a
        // failed mount (no FAT) leaves FatFs pointing at fs, which esp_vfs_fat_unregister_path is about to free
        f_mount(NULL, drv, 0);
        esp_vfs_fat_unregister_path(SD_BASE); // frees fs
        vfs_up = false;
        fs     = NULL;
    }
    if (pdrv != 0xFF) {
        ff_diskio_unregister(pdrv);
        pdrv = 0xFF;
    }
    if (slot_up) {
        // Slot 0 only: the controller stays while the C6 link holds slot 1. If the lock does not come, deinit anyway:
        // leaving the slot behind is worse than the (unlikely) race
        bool locked = board_sdmmc_lock(SD_HOST_WAIT_MS);
        sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_0);
        if (locked) {
            board_sdmmc_unlock();
        }
        slot_up = false;
    }
    free(card);
    card = NULL;
    if (pwr) {
        esp_err_t err = sd_pwr_ctrl_del_on_chip_ldo(pwr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LDO %d not released: %s", SD_LDO_CHAN, esp_err_to_name(err)); // the handle leaks
        }
        pwr = NULL;
    }
    heap_caps_free(bounce);
    bounce = NULL;
}

// After a failed slot init: a controller we just made has no slot at all, so nothing frees it (deinit_slot needs a
// slot). Delete it only when no slot is registered, so the C6 link's slot 1 is never touched. With the lock held.
static void drop_empty_controller(void) {
    sdmmc_host_state_t state = {0};
    if (sdmmc_host_get_state(&state) == ESP_OK && state.host_initialized && state.num_of_init_slots == 0) {
        sdmmc_host_deinit();
    }
}

static sd_mount_outcome_t power_and_probe(char *reason, size_t cap) {
    sd_pwr_ctrl_ldo_config_t const ldo = {.ldo_chan_id = SD_LDO_CHAN};
    esp_err_t                      err = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &pwr);
    if (err != ESP_OK) {
        pwr = NULL;
        set_reason(reason, cap, "LDO", err);
        return SD_MOUNT_ERROR;
    }
    // heap_caps adds the cache-line alignment (128 B on the P4's L2) for a DMA buffer in PSRAM
    bounce = heap_caps_calloc(1, SD_BOUNCE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    card   = calloc(1, sizeof(*card)); // esp_vfs_fat does the same; small
    if (!bounce || !card) {
        snprintf(reason, cap, "out of memory");
        return SD_MOUNT_ERROR;
    }
    vTaskDelay(pdMS_TO_TICKS(SD_POWER_UP_MS));

    sdmmc_host_t host       = SDMMC_HOST_DEFAULT(); // keeps SDMMC_HOST_FLAG_DEINIT_ARG: deinit is per slot
    host.slot               = SDMMC_HOST_SLOT_0;
    host.max_freq_khz       = SDMMC_FREQ_DEFAULT; // 20 MHz, as BadgeVMS (Lucas, #60)
    host.pwr_ctrl_handle    = pwr;
    host.dma_aligned_buffer = bounce;
    // Width 4 explicitly: the default (0) means the slot's maximum, 8, and slot 0's D5 is GPIO46, which is tied to GND
    // on production boards. With width 4 the driver never configures D4-D7. No UHS-I: VO4 is the card's VDD too, a
    // 1.8 V switch would undervolt the card itself.
    sdmmc_slot_config_t const slot = {
        .clk   = GPIO_NUM_43,
        .cmd   = GPIO_NUM_44,
        .d0    = GPIO_NUM_39,
        .d1    = GPIO_NUM_40,
        .d2    = GPIO_NUM_41,
        .d3    = GPIO_NUM_42,
        .d4    = GPIO_NUM_NC,
        .d5    = GPIO_NUM_NC,
        .d6    = GPIO_NUM_NC,
        .d7    = GPIO_NUM_NC,
        .cd    = SDMMC_SLOT_NO_CD,
        .wp    = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP, // on top of the board's 51K pull-ups; harmless
    };
    // Under the controller lock, so esp_hosted cannot delete the controller between our init and our slot (#60)
    if (!board_sdmmc_lock(SD_HOST_WAIT_MS)) {
        snprintf(reason, cap, "SDMMC busy (C6 link)");
        return SD_MOUNT_ERROR;
    }
    err = sdmmc_host_init(); // ESP_OK when esp_hosted already made the controller
    if (err == ESP_OK) {
        err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_0, &slot);
        if (err != ESP_OK) {
            drop_empty_controller();
        }
    }
    board_sdmmc_unlock();
    if (err != ESP_OK) {
        set_reason(reason, cap, "host", err);
        return SD_MOUNT_ERROR;
    }
    slot_up = true;
    err     = sdmmc_card_init(&host, card);
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_RESPONSE) {
        set_reason(reason, cap, "no answer", err);
        return SD_MOUNT_NO_CARD;
    }
    if (err != ESP_OK) {
        set_reason(reason, cap, "card init", err);
        return SD_MOUNT_ERROR;
    }
    publish_card();
    return SD_MOUNT_OK;
}

// FatFs picks FAT12/16/32 and the cluster size from the card size (au_size 0). esp_vfs_fat's own format would force
// 512-byte clusters (its allocation unit is at least one sector), which on a 32 GB card means a 250 MB FAT.
static FRESULT make_fat(void) {
    void *work = heap_caps_calloc(1, SD_MKFS_WORK, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!work) {
        return FR_NOT_ENOUGH_CORE;
    }
    // FM_ANY never means exFAT here: FF_FS_EXFAT is 0 in IDF's ffconf.h, so a big card gets FAT32
    MKFS_PARM const opt = {.fmt = FM_ANY, .n_fat = 2, .align = 0, .n_root = 0, .au_size = 0};
    f_mount(NULL, drv, 0); // f_mkfs invalidates whatever FatFs has registered for this volume: make sure it is nothing
    ESP_LOGW(TAG, "formatting the card");
    FRESULT res = f_mkfs(drv, &opt, work, SD_MKFS_WORK);
    heap_caps_free(work);
    return res;
}

static sd_mount_outcome_t bring_up(bool format, char *reason, size_t cap) {
    sd_mount_outcome_t outcome = power_and_probe(reason, cap);
    if (outcome != SD_MOUNT_OK) {
        return outcome;
    }
    if (ff_diskio_get_drive(&pdrv) != ESP_OK) {
        pdrv = 0xFF;
        snprintf(reason, cap, "no free FAT volume");
        return SD_MOUNT_ERROR;
    }
    ff_diskio_register_sdmmc(pdrv, card);
    ff_sdmmc_set_disk_status_check(pdrv, true); // CMD13 per access: a pulled card fails fast instead of timing out
    drv[0] = (char)('0' + pdrv);
    drv[1] = ':';
    drv[2] = '\0';
    esp_vfs_fat_conf_t const conf = {.base_path = SD_BASE, .fat_drive = drv, .max_files = SD_MAX_FILES};
    esp_err_t                err  = esp_vfs_fat_register(&conf, &fs);
    if (err != ESP_OK) {
        set_reason(reason, cap, "vfs", err);
        return SD_MOUNT_ERROR;
    }
    vfs_up      = true;
    FRESULT res = format ? make_fat() : FR_OK;
    if (res != FR_OK) {
        snprintf(reason, cap, "format failed (FR %d)", (int)res);
        return SD_MOUNT_ERROR;
    }
    res = f_mount(fs, drv, 1);
    if (res == FR_NO_FILESYSTEM) {
        return SD_MOUNT_NOT_FAT;
    }
    if (res != FR_OK) {
        snprintf(reason, cap, "mount failed (FR %d)", (int)res);
        return SD_MOUNT_ERROR;
    }
#if CONFIG_FATFS_USE_LABEL
    if (format) {
        char label[12];
        snprintf(label, sizeof(label), "%sLICHEN", drv);
        res = f_setlabel(label);
        if (res != FR_OK) {
            ESP_LOGW(TAG, "volume label not set (FR %d)", (int)res);
        }
    }
#endif
    if (mkdir(SD_LICHEN_DIR, 0755) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "%s: mkdir failed (errno %d)", SD_LICHEN_DIR, errno);
    }
    return SD_MOUNT_OK;
}

// With `lock` held. Logs one line either way: the default log level hides ESP_LOGI, and the bench wants to see it.
static bool mount_locked(bool format) {
    char reason[sizeof(info.reason)] = "";
    portENTER_CRITICAL(&info_lock);
    memset(&info, 0, offsetof(sd_info_t, generation)); // keeps the generation
    memset(info.reason, 0, sizeof(info.reason));
    portEXIT_CRITICAL(&info_lock);

    sd_mount_outcome_t outcome = bring_up(format, reason, sizeof(reason));
    sd_mount_plan_t    plan    = sd_policy_after_mount(outcome);
    if (!plan.keep_power) {
        bring_down();
    }
    portENTER_CRITICAL(&info_lock);
    info.state = plan.state;
    strlcpy(info.reason, reason, sizeof(info.reason));
    if (plan.state == SD_STATE_MOUNTED) {
        info.generation++;
    }
    portEXIT_CRITICAL(&info_lock);
    if (plan.state == SD_STATE_MOUNTED) {
        io = (sd_io_count_t){0};
        refresh_space();
        atomic_store(&mounted, true);
    }
    sd_info_t now;
    sd_info(&now);
    char line[128];
    sd_status_line(&now, line, sizeof(line));
    if (plan.state == SD_STATE_NO_CARD) {
        ESP_LOGW(TAG, "%s [%s]", line, reason); // the reason tells "no card" from a card that does not answer
    } else {
        ESP_LOGW(TAG, "%s", line);
    }
    return plan.state == SD_STATE_MOUNTED;
}

static bool take(uint32_t wait_ms) {
    return lock && xSemaphoreTake(lock, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

bool sd_mount(void) {
    if (!take(SD_LOCK_WAIT_MS)) {
        return false;
    }
    bool ok = atomic_load(&mounted) || mount_locked(false);
    xSemaphoreGive(lock);
    return ok;
}

void sd_unmount(void) {
    if (!take(SD_LOCK_WAIT_MS)) {
        ESP_LOGE(TAG, "unmount: the card stays busy");
        return;
    }
    portENTER_CRITICAL(&info_lock); // readers never see MOUNTED once mounted is false
    info.state      = SD_STATE_OFF;
    info.free_known = false;
    atomic_store(&mounted, false);
    portEXIT_CRITICAL(&info_lock);
    bring_down();
    xSemaphoreGive(lock);
}

bool sd_format(void) {
    if (!take(SD_LOCK_WAIT_MS)) {
        return false;
    }
    bring_down();
    bool ok = mount_locked(true);
    xSemaphoreGive(lock);
    return ok;
}

bool sd_mounted(void) {
    return atomic_load(&mounted);
}

bool sd_begin(uint32_t wait_ms) {
    if (!atomic_load(&mounted) || !take(wait_ms)) {
        return false;
    }
    if (!atomic_load(&mounted)) { // unmounted while we waited
        xSemaphoreGive(lock);
        return false;
    }
    return true;
}

void sd_end(sd_io_t result) {
    if (sd_policy_io(&io, result)) {
        ESP_LOGW(TAG, "%d failed batches in a row: unmounting the card", SD_IO_ERRORS_TO_UNMOUNT);
        portENTER_CRITICAL(&info_lock);
        info.state      = SD_STATE_REMOVED;
        info.io_errors  = io.total;
        info.free_known = false;
        atomic_store(&mounted, false);
        portEXIT_CRITICAL(&info_lock);
        bring_down();
    } else {
        portENTER_CRITICAL(&info_lock);
        info.io_errors = io.total;
        portEXIT_CRITICAL(&info_lock);
        if (result != SD_IO_FAILED) {
            refresh_space();
        }
    }
    xSemaphoreGive(lock);
}

void sd_info(sd_info_t *out) {
    portENTER_CRITICAL(&info_lock);
    *out = info;
    portEXIT_CRITICAL(&info_lock);
}

void sd_console_register(void); // sd_console.c

void sd_start(void) {
    if (lock) {
        return; // once per boot
    }
    lock = xSemaphoreCreateMutexStatic(&lock_buf);
    sd_console_register();
    sd_mount();
}
