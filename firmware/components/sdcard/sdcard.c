// The SD card (#60): power, probe, FAT mount and the batch mutex. The pins, LDO channel and slot are checked against
// IDF 6.1 (esp_hal_sd/esp32p4/include/soc/sdmmc_pins.h: slot 0 IO-MUX = CLK 43, CMD 44, D0-D3 39-42) and the
// hardware notes on issue #60 (BadgeVMS drivers/fatfs.c, the M.2 card's schematic: VO4 = V/SDMMC).
#include "sdcard.h"

#include "board_sdmmc.h"
#include "diskio_impl.h"
#include "sd_bounce.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_protocol_defs.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "psram_dma_cmd.h"
#include "sdmmc_cmd.h"

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char const TAG[] = "sd";

#define SD_LDO_CHAN     4   // LDO_VO4: VDD_IO_5 (GPIO39-48) and the card's VDD
#define SD_POWER_UP_MS  10  // card VDD ramp before the first command
#define SD_BOUNCE_ALIGN 128 // the L2 cache line: the bounce buffer shares no line with anything else
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
static uint8_t             *bounce; // internal DMA RAM (sd_bounce.h), bounce_bytes long
static size_t               bounce_bytes;
static bool                 slot_up;
static sdmmc_card_t        *card;
static BYTE                 pdrv = 0xFF;
static char                 drv[3];
static FATFS               *fs;
static bool                 vfs_up;

// The bounce buffer is shared by FatFs (its volume lock serialises it) and the bounced test leg of "fw psram dma"
// (under sd_begin only): this keeps the two apart
static SemaphoreHandle_t bounce_lock;
static StaticSemaphore_t bounce_lock_buf;

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

// Slot 0's pins (sdmmc_pins.h, IO-MUX): CLK 43, CMD 44, D0-D3 39-42
static gpio_num_t const SD_PINS[] = {GPIO_NUM_43, GPIO_NUM_44, GPIO_NUM_39, GPIO_NUM_40, GPIO_NUM_41, GPIO_NUM_42};

// IDF's sdmmc_host_deinit_slot always tries to delete the controller after removing the slot. While the C6 link's
// slot 1 is registered that fails by design (deinit_slot maps it to ESP_OK), but SD_HOST logs it at E level on every
// unmount (#100): that one expected line is muted when another slot is registered. The controller is never deleted
// from here while slot 1 is on it (IDF checks the slot count under its own mutex). Then the six pins are reset: the
// slot leaves D3 reserved as a GPIO output (a "conflict found for GPIO[42]" warning on the next mount), and the pins
// stop being SDMMC outputs before VO4 drops, so nothing drives the card's data lines while it has no power.
static void deinit_slot0(void) {
    sdmmc_host_state_t state  = {0};
    bool               shared = sdmmc_host_get_state(&state) == ESP_OK && state.num_of_init_slots > 1;
    esp_log_level_t    was    = esp_log_level_get("SD_HOST");
    if (shared) {
        esp_log_level_set("SD_HOST", ESP_LOG_NONE);
    }
    esp_err_t err = sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_0);
    if (shared) {
        esp_log_level_set("SD_HOST", was);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "slot 0 deinit: %s", esp_err_to_name(err)); // muted above, so said here
    }
    for (size_t i = 0; i < sizeof(SD_PINS) / sizeof(SD_PINS[0]); i++) {
        gpio_reset_pin(SD_PINS[i]);
    }
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
        deinit_slot0();
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
    bounce       = NULL;
    bounce_bytes = 0;
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
    // Internal DMA RAM (#121: no SDMMC DMA into PSRAM); smaller on a tight heap, one card command per chunk either way.
    // When every size fails the loop ends with bounce_bytes 0, so it never names a buffer that is not there.
    for (unsigned attempt = 0; (bounce_bytes = sd_bounce_try_size(attempt)) != 0; attempt++) {
        uint32_t caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        bounce        = heap_caps_aligned_calloc(SD_BOUNCE_ALIGN, 1, bounce_bytes, caps);
        if (bounce) {
            break;
        }
    }
    if (bounce && bounce_bytes < SD_BOUNCE_BYTES) {
        ESP_LOGW(TAG, "bounce buffer %u KB (internal DMA RAM is short)", (unsigned)(bounce_bytes >> 10));
    }
    card = calloc(1, sizeof(*card)); // esp_vfs_fat does the same; small
    if (!bounce || !card) {
        snprintf(reason, cap, "out of memory");
        return SD_MOUNT_ERROR;
    }
    vTaskDelay(pdMS_TO_TICKS(SD_POWER_UP_MS));

    sdmmc_host_t host       = SDMMC_HOST_DEFAULT(); // keeps SDMMC_HOST_FLAG_DEINIT_ARG: deinit is per slot
    host.slot               = SDMMC_HOST_SLOT_0;
    host.max_freq_khz       = SDMMC_FREQ_DEFAULT; // 20 MHz, as BadgeVMS (Lucas, #60)
    host.pwr_ctrl_handle    = pwr;
    // No dma_aligned_buffer: sd_disk_read/write never hand IDF a buffer it would have to bounce itself (IDF's bounce
    // takes its chunk length from heap_caps_get_allocated_size, which need not be a cache-line multiple)
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

// ---- the SD volume's FatFs disk driver (#121) ------------------------------------------------------------------
// Instead of IDF's ff_diskio_register_sdmmc: the same card calls, but a buffer in PSRAM (FatFs hands the caller's
// buffer through for whole-sector reads and writes: the journal's 32 KB reads, the Files app, f_mkfs) or one the SDMMC
// DMA cannot use goes through the internal bounce buffer, so the SDMMC controller never DMAs to or from PSRAM. FatFs's
// own window and file buffers are in PSRAM too (CONFIG_FATFS_ALLOC_PREFER_EXTRAM, #263) and bounce the same way; only
// when PSRAM is full does ff_memalloc fall back to internal DMA RAM, which then takes the direct path.

static esp_err_t bounced(uint8_t *buf, uint32_t sector, uint32_t count, bool write) {
    sd_bounce_iter_t  it;
    sd_bounce_chunk_t c;
    if (!bounce || !sd_bounce_start(&it, sector, count, card->csd.sector_size, bounce_bytes)) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(bounce_lock, portMAX_DELAY);
    while (err == ESP_OK && sd_bounce_next(&it, &c)) {
        if (write) {
            memcpy(bounce, buf + c.offset, c.bytes);
            err = sdmmc_write_sectors(card, bounce, c.sector, c.count); // internal and aligned: IDF's direct DMA path
        } else {
            err = sdmmc_read_sectors(card, bounce, c.sector, c.count);
            if (err == ESP_OK) {
                memcpy(buf + c.offset, bounce, c.bytes);
            }
        }
    }
    xSemaphoreGive(bounce_lock);
    return err;
}

static esp_err_t sd_xfer(uint8_t *buf, uint32_t sector, uint32_t count, bool write) {
    size_t len     = (size_t)count * card->csd.sector_size;
    bool   direct = esp_ptr_dma_capable(buf) && card->host.check_buffer_alignment(card->host.slot, buf, len);
    if (sd_bounce_needed(esp_ptr_external_ram(buf), direct)) {
        return bounced(buf, sector, count, write);
    }
    return write ? sdmmc_write_sectors(card, buf, sector, count) : sdmmc_read_sectors(card, buf, sector, count);
}

// CMD13 per access, as ff_sdmmc_set_disk_status_check(true) did: a pulled card fails fast instead of timing out
static DSTATUS sd_disk_status(BYTE drive) {
    (void)drive;
    if (!card) {
        return STA_NOINIT;
    }
    esp_err_t err = sdmmc_get_status(card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "card status: %s", esp_err_to_name(err));
        return STA_NOINIT;
    }
    return 0;
}

static DRESULT sd_disk_read(BYTE drive, BYTE *buf, LBA_t sector, UINT count) {
    (void)drive;
    if (!card) {
        return RES_NOTRDY;
    }
    esp_err_t err = sd_xfer(buf, (uint32_t)sector, count, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read %u+%u: %s", (unsigned)sector, (unsigned)count, esp_err_to_name(err));
    }
    return err == ESP_OK ? RES_OK : RES_ERROR;
}

static DRESULT sd_disk_write(BYTE drive, BYTE const *buf, LBA_t sector, UINT count) {
    (void)drive;
    if (!card) {
        return RES_NOTRDY;
    }
    esp_err_t err = sd_xfer((uint8_t *)buf, (uint32_t)sector, count, true); // bounced() only reads from buf on a write
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write %u+%u: %s", (unsigned)sector, (unsigned)count, esp_err_to_name(err));
    }
    return err == ESP_OK ? RES_OK : RES_ERROR;
}

static DRESULT sd_disk_ioctl(BYTE drive, BYTE cmd, void *buf) {
    (void)drive;
    if (!card) {
        return RES_NOTRDY;
    }
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK; // every write is complete when sdmmc_write_sectors returns
    case GET_SECTOR_COUNT:
        *(LBA_t *)buf = card->csd.capacity;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buf = (WORD)card->csd.sector_size;
        return RES_OK;
#if FF_USE_TRIM
    case CTRL_TRIM: {
        if (sdmmc_can_trim(card) != ESP_OK) {
            return RES_PARERR;
        }
        LBA_t const      *range = buf; // first and last sector, inclusive
        sdmmc_erase_arg_t arg   = sdmmc_can_discard(card) == ESP_OK ? SDMMC_DISCARD_ARG : SDMMC_ERASE_ARG;
        return sdmmc_erase_sectors(card, range[0], range[1] - range[0] + 1, arg) == ESP_OK ? RES_OK : RES_ERROR;
    }
#endif
    default:
        return RES_ERROR; // GET_BLOCK_SIZE included, as IDF's driver: FatFs then aligns nothing to erase blocks
    }
}

static ff_diskio_impl_t const SD_DISKIO = {
    .init   = sd_disk_status,
    .status = sd_disk_status,
    .read   = sd_disk_read,
    .write  = sd_disk_write,
    .ioctl  = sd_disk_ioctl,
};

// FatFs picks FAT12/16/32 and the cluster size from the card size (au_size 0). esp_vfs_fat's own format would force
// 512-byte clusters (its allocation unit is at least one sector), which on a 32 GB card means a 250 MB FAT.
static FRESULT make_fat(void) {
    void *work = heap_caps_calloc(1, SD_MKFS_WORK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); // bounced by sd_xfer
    if (!work) {
        return FR_NOT_ENOUGH_CORE;
    }
    // FM_ANY never means exFAT here: FF_FS_EXFAT is 0 in IDF's ffconf.h, so a big card gets FAT32
    MKFS_PARM const opt = {.fmt = FM_ANY, .n_fat = 2, .align = 0, .n_root = 0, .au_size = 0};
    f_mount(NULL, drv, 0); // f_mkfs invalidates whatever FatFs has registered for this volume: make sure it is nothing
    ESP_LOGI(TAG, "formatting the card");
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
    ff_diskio_register(pdrv, &SD_DISKIO);
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
    // No card or a mounted one is normal; a card that is there but unusable is not (#113)
    bool normal = plan.state == SD_STATE_MOUNTED || plan.state == SD_STATE_NO_CARD || plan.state == SD_STATE_OFF;
    esp_log_level_t level = normal ? ESP_LOG_INFO : ESP_LOG_WARN;
    if (plan.state == SD_STATE_NO_CARD) {
        // the reason tells "no card" from a card that does not answer
        ESP_LOG_LEVEL(level, TAG, "%s [%s]", line, reason);
    } else {
        ESP_LOG_LEVEL(level, TAG, "%s", line);
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
        ESP_LOGW(TAG, "unmount: the card stays busy");
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

// Raw, read-only sector reads for "fw psram dma" (#121): the SDMMC -> PSRAM DMA path of #120 without FatFs. The
// caller holds the batch mutex through these begin/end wrappers.
static bool raw_begin(uint32_t wait_ms) {
    return sd_begin(wait_ms);
}

// A failed test read is reported by the test itself; it does not count toward the card's auto-unmount
static void raw_end(bool failed) {
    (void)failed;
    sd_end(SD_IO_NEUTRAL);
}

// The direct leg: sdmmc_read_sectors DMAs straight into an aligned PSRAM dst, the #120 path FatFs no longer takes
static bool raw_read(void *dst, uint32_t sector, uint32_t count) {
    return card && sdmmc_read_sectors(card, dst, sector, count) == ESP_OK;
}

// The bounced leg: the same sectors through the FatFs driver's own path (internal bounce buffer, then a CPU copy)
static bool raw_read_bounced(void *dst, uint32_t sector, uint32_t count) {
    return card && bounced(dst, sector, count, false) == ESP_OK;
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
        ESP_LOGE(TAG, "%d failed batches in a row: unmounting the card", SD_IO_ERRORS_TO_UNMOUNT);
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
    lock        = xSemaphoreCreateMutexStatic(&lock_buf);
    bounce_lock = xSemaphoreCreateMutexStatic(&bounce_lock_buf);
    sd_console_register();
    psram_dma_set_sd(&(psram_dma_sd_t){.begin = raw_begin, .end = raw_end, .read = raw_read, .read_bounced = raw_read_bounced,
                                        .mounted = sd_mounted});
    sd_mount();
}
