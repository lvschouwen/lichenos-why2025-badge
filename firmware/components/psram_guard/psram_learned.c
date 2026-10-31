#include "psram_learned.h"

#include "esp_log.h"
#include "fw_console.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#define KEY "psram_lrn"

#ifdef CONFIG_SPIRAM_SPEED
#define SPEED CONFIG_SPIRAM_SPEED
#else
#define SPEED 0
#endif

static char const TAG[] = "psram";

static psram_learn_t     s_rec;
static SemaphoreHandle_t s_lock;
static unsigned          s_outliers;
static unsigned          s_loaded_writes;
static bool              s_from_nvs;
static char              s_last_who[24];

static bool store(void) {
    nvs_handle_t h;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_blob(h, KEY, &s_rec, sizeof(s_rec)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void psram_learned_load(void) {
    s_lock = xSemaphoreCreateMutex();
    psram_learn_reset(&s_rec, SPEED);
    if (nvs_flash_init() != ESP_OK) { // unusable here: app_main erases and retries later, the range starts empty
        return;
    }
    nvs_handle_t h;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    psram_learn_t rec  = {0};
    size_t        size = sizeof(rec);
    esp_err_t     err  = nvs_get_blob(h, KEY, &rec, &size);
    nvs_close(h);
    if (err == ESP_OK && psram_learn_usable(&rec, size, SPEED)) {
        s_rec           = rec;
        s_from_nvs      = true;
        s_loaded_writes = rec.writes;
    }
}

void psram_learned_feed(uintptr_t lo, uintptr_t hi, char const *who) {
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    psram_learn_result_t r = psram_learn_note(&s_rec, lo, hi);
    if (r == PSRAM_LEARN_OUTLIER) {
        s_outliers++;
        ESP_LOGW(TAG, "learned: %s found bad bytes %p..%p, too far from the learned range: not learned", who, (void *)lo,
                 (void *)hi);
    } else if (r == PSRAM_LEARN_GREW) {
        bool ok = store();
        snprintf(s_last_who, sizeof(s_last_who), "%s", who);
        ESP_LOGW(TAG, "learned: %s widened the bad range to 0x%08x..0x%08x (%u KB), NVS write #%u %s", who, (unsigned)s_rec.lo,
                 (unsigned)s_rec.hi, (unsigned)((s_rec.hi - s_rec.lo + 1) >> 10), (unsigned)s_rec.writes,
                 ok ? "stored" : "FAILED");
    }
    xSemaphoreGive(s_lock);
}

psram_learn_t psram_learned_get(void) {
    psram_learn_t r;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    r = s_rec;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return r;
}

void psram_learned_forget(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    psram_learn_reset(&s_rec, SPEED);
    nvs_handle_t h;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, KEY);
        nvs_commit(h);
        nvs_close(h);
    }
    s_from_nvs = false;
    s_outliers = 0;
    xSemaphoreGive(s_lock);
    console_reply("psram learned range forgotten (the quarantine of this boot stays; the next boot scans from scratch)");
}

void psram_learned_print(void) {
    psram_learn_t r = psram_learned_get();
    if (psram_learn_empty(&r)) {
        console_reply("psram learned: nothing yet (speed %u MHz)", (unsigned)r.speed_mhz);
        return;
    }
    console_reply("psram learned: 0x%08x..0x%08x (%u KB span), speed %u MHz, NVS writes %u (%u at boot, %u this boot), "
                  "last widened by %s, %u outliers ignored, loaded from NVS %s",
                  (unsigned)r.lo, (unsigned)r.hi, (unsigned)((r.hi - r.lo + 1) >> 10), (unsigned)r.speed_mhz,
                  (unsigned)r.writes, s_loaded_writes, (unsigned)(r.writes - s_loaded_writes),
                  s_last_who[0] ? s_last_who : "-", s_outliers, s_from_nvs ? "yes" : "no");
}
