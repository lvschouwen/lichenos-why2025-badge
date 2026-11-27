// The P4 image from the SD card at boot (#595): see ota_sd_update.h. The rule is sd_update_policy.c (host-tested).
#include "ota_sd_update.h"

#include "fw_console.h"
#include "ota_trial.h"
#include "sd_update_policy.h"
#include "sdcard.h"
#include "sha256.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static char const TAG[] = "sd_update";

#define SDU_FILE     OTA_SD_UPDATE_DIR "/badge_fw.bin"
#define SDU_SHA_FILE SDU_FILE ".sha256"
#define SDU_PIECE    4096 // the bounce buffer: internal RAM, only while an install runs
#define SDU_WAIT_MS  2000 // the card's batch lock; nothing else uses the card this early

_Static_assert(sizeof(((ota_sd_record_t *)0)->elf_sha) == SDU_SHA_LEN, "record sha size");

static ota_sd_update_hooks_t s_hooks;

typedef struct {
    FILE    *f;
    long     size;
    uint8_t  elf_sha[SDU_SHA_LEN]; // from the card file's app descriptor
    uint8_t  digest[SDU_SHA_LEN];  // SHA-256 of the whole card file
    uint8_t *buf;                  // SDU_PIECE bytes, internal
    int      err;                  // errno of a failed card read, for sd_end
} sdu_job_t;

static void notice(bool warn, char const *title, char const *body) {
    ESP_LOG_LEVEL(warn ? ESP_LOG_WARN : ESP_LOG_INFO, TAG, "%s: %s", title, body);
    if (s_hooks.notice) {
        s_hooks.notice(warn, title, body);
    }
}

static bool tried_read(uint8_t out[SDU_SHA_LEN], bool *told) {
    ota_sd_record_t r;
    *told = false;
    if (!s_hooks.record_get || !s_hooks.record_get(&r)) {
        return false;
    }
    memcpy(out, r.elf_sha, SDU_SHA_LEN);
    *told = r.told != 0;
    return true;
}

// sha NULL clears the record (all zero); a new record starts untold. True only when saved.
static bool tried_write(uint8_t const *sha) {
    ota_sd_record_t r = {0};
    if (sha) {
        memcpy(r.elf_sha, sha, SDU_SHA_LEN);
    }
    return s_hooks.record_set && s_hooks.record_set(&r);
}

static void told_write(void) {
    ota_sd_record_t r;
    if (s_hooks.record_get && s_hooks.record_get(&r)) {
        r.told = 1;
        s_hooks.record_set(&r);
    }
}

// The cheap look: the image header and the app descriptor (~200 bytes), not the whole file
static sdu_card_t read_head(sdu_job_t *j) {
    j->f = fopen(SDU_FILE, "rb");
    if (!j->f) {
        j->err = errno == ENOENT ? 0 : errno;
        return errno == ENOENT ? SDU_CARD_ABSENT : SDU_CARD_UNREADABLE;
    }
    uint8_t head[SDU_HEAD_LEN];
    size_t  n = fread(head, 1, sizeof(head), j->f);
    if (n < sizeof(head) && ferror(j->f)) {
        j->err = errno;
    }
    return sdu_card_from_head(head, n, CONFIG_IDF_FIRMWARE_CHIP_ID, j->elf_sha);
}

// Pass 1: the size and SHA-256 of the whole file, before anything is erased
static bool hash_file(sdu_job_t *j) {
    if (fseek(j->f, 0, SEEK_END) != 0 || (j->size = ftell(j->f)) <= 0 || fseek(j->f, 0, SEEK_SET) != 0) {
        j->err = errno;
        return false;
    }
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    long   total = 0;
    size_t n;
    while ((n = fread(j->buf, 1, SDU_PIECE, j->f)) > 0) {
        sha256_update(&ctx, j->buf, n);
        total += (long)n;
    }
    if (ferror(j->f) || total != j->size) {
        j->err = errno ? errno : EIO;
        return false;
    }
    sha256_final(&ctx, j->digest);
    return true;
}

static bool sha_file_matches(sdu_job_t *j) {
    char  text[160];
    FILE *f = fopen(SDU_SHA_FILE, "r");
    if (!f) {
        return false;
    }
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    return sdu_sha256_text_ok(text, n, j->digest);
}

// Pass 2: card -> slot in SDU_PIECE pieces. A card pulled here ends in esp_ota_abort: the boot slot is never touched.
static bool write_slot(sdu_job_t *j, esp_partition_t const *slot) {
    esp_ota_handle_t h;
    if ((size_t)j->size > slot->size || fseek(j->f, 0, SEEK_SET) != 0 || esp_ota_begin(slot, j->size, &h) != ESP_OK) {
        return false;
    }
    long done = 0;
    int  last = -1;
    while (done < j->size) {
        size_t n = fread(j->buf, 1, SDU_PIECE, j->f);
        if (n == 0 || esp_ota_write(h, j->buf, n) != ESP_OK) {
            j->err = n == 0 ? (errno ? errno : EIO) : 0;
            esp_ota_abort(h);
            return false;
        }
        done += (long)n;
        int pct = (int)(done * 100 / j->size);
        if (pct / 25 != last / 25) {
            ESP_LOGI(TAG, "writing %s: %d%%", slot->label, pct);
            last = pct;
        }
    }
    return esp_ota_end(h) == ESP_OK; // checks the image: chip, segments, hash
}

// The slot read back (esp_partition_read, no mmap: #31) against the card file's SHA-256. esp_partition_get_sha256 is
// no read-back here: for an app image it returns the hash appended to the image, which is not the file's.
static bool slot_matches(sdu_job_t *j, esp_partition_t const *slot) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    for (long off = 0; off < j->size; off += SDU_PIECE) {
        size_t n = (size_t)(j->size - off < SDU_PIECE ? j->size - off : SDU_PIECE);
        if (esp_partition_read(slot, (size_t)off, j->buf, n) != ESP_OK) {
            return false;
        }
        sha256_update(&ctx, j->buf, n);
    }
    uint8_t got[SDU_SHA_LEN];
    sha256_final(&ctx, got);
    return memcmp(got, j->digest, SDU_SHA_LEN) == 0;
}

static char const *install_steps(sdu_job_t *j, esp_partition_t const *slot) {
    if (!hash_file(j)) {
        return "badge_fw.bin could not be read";
    }
    if (!sha_file_matches(j)) {
        return "badge_fw.bin does not match badge_fw.bin.sha256";
    }
    if (!slot || !write_slot(j, slot)) {
        return j->err ? "the card could not be read while writing" : "the image could not be written";
    }
    if (!slot_matches(j, slot)) {
        return "the written image did not read back the same";
    }
    // Before the boot slot changes: a rollback brings the badge back here, and the record stops a second try
    if (!tried_write(j->elf_sha)) {
        return "the attempt could not be recorded";
    }
    if (esp_ota_set_boot_partition(slot) != ESP_OK) {
        tried_write(NULL); // never made the boot slot: not tried
        return "the new image could not be made the boot slot";
    }
    ota_image_note_slot_switch(slot); // a card is physical access: its trial needs no Wi-Fi (#411)
    return NULL;
}

static bool install(sdu_job_t *j) {
    j->buf = heap_caps_malloc(SDU_PIECE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!j->buf) {
        notice(true, "SD card update not installed", "no memory for the copy");
        return false;
    }
    esp_partition_t const *slot = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "installing " SDU_FILE " into %s", slot ? slot->label : "?");
    int64_t     t0  = esp_timer_get_time();
    char const *why = install_steps(j, slot);
    heap_caps_free(j->buf);
    j->buf = NULL;
    if (why) {
        notice(true, "SD card update not installed", why);
        return false;
    }
    ESP_LOGI(TAG, "installed into %s in %u ms, restarting", slot->label,
             (unsigned)((esp_timer_get_time() - t0) / 1000));
    return true;
}

static void report_skip(sdu_action_t a, bool told) {
    ESP_LOGI(TAG, "card image: %s", sdu_action_name(a));
    if (!sdu_action_notice(a, told)) {
        return;
    }
    if (a == SDU_SKIP_TRIED) {
        notice(true, "SD card update not installed again",
               "This image was installed from the card before and another one runs now (a rollback or a USB flash)");
        told_write();
    } else {
        notice(true, "SD card update ignored", a == SDU_SKIP_NOT_APP ? "badge_fw.bin is not a badge image"
                                                                       : "badge_fw.bin could not be read");
    }
}

void ota_sd_update_boot(ota_sd_update_hooks_t const *hooks) {
    s_hooks = hooks ? *hooks : (ota_sd_update_hooks_t){0};
    if (!sd_mounted() || !s_hooks.record_get || !s_hooks.record_set) {
        return;
    }
    if (!sd_begin(SDU_WAIT_MS)) {
        ESP_LOGW(TAG, "card busy: no look for an update this boot");
        return;
    }
    int64_t    t0   = esp_timer_get_time();
    sdu_job_t  j    = {0};
    sdu_card_t card = read_head(&j);
    uint8_t    tried[SDU_SHA_LEN];
    bool       told = false;
    bool       have = tried_read(tried, &told) && memcmp(tried, (uint8_t[SDU_SHA_LEN]){0}, SDU_SHA_LEN) != 0;
    bool       ok   = card == SDU_CARD_OK;
    uint8_t const *running = esp_app_get_description()->app_elf_sha256;
    sdu_action_t   a = sdu_decide(card, ok && memcmp(j.elf_sha, running, SDU_SHA_LEN) == 0,
                                  ok && have && memcmp(j.elf_sha, tried, SDU_SHA_LEN) == 0, ota_image_trial_pending());
    ESP_LOGI(TAG, "card check %u us", (unsigned)(esp_timer_get_time() - t0));
    bool installed = a == SDU_INSTALL && install(&j);
    if (a != SDU_INSTALL) {
        report_skip(a, told);
    }
    if (j.f) {
        fclose(j.f);
    }
    sd_end(j.err ? sd_io_from_errno(j.err) : SD_IO_OK);
    if (a == SDU_INSTALL) { // the bench reads it (#595): esp_ota_end and the flash writes run on the boot task
        ESP_LOGI(TAG, "boot task stack: %u B never used", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    if (installed) {
        system_restart_asked("SD card update installed"); // asked: an unread trial state must not hold it (#304)
    }
}
