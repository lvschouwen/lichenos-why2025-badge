/*
 * IDENT at boot (#36): which app does the C6 run? The answer lets a warm boot trust the C6 flash without the slow ROM
 * check. Runs before lora_start, so before the LoRa task exists: it sends on its own and borrows the peer-data callback.
 */

#include "lora.h"

#include "eh_host_feat_peer_data.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lora_proto.h"

#include <string.h>

_Static_assert(LORA_IDENT_LEN == LORA_IDENT_SHA_LEN, "IDENT sha size");

#define IDENT_RETRY_MS 250

static char const        TAG[] = "lora";
static SemaphoreHandle_t ident_done;
static portMUX_TYPE      ident_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t           ident_sha[LORA_IDENT_LEN];

// The default event loop's task: copy and get out. Anything but an IDENT is dropped (nothing else is asked yet).
static void on_ident(uint32_t msg_id, uint8_t const *data, size_t len, void *ctx) {
    (void)ctx;
    static lora_msg_t msg; // only this callback, on one task: off its small stack
    if (msg_id != LORA_MSG_ID_TO_P4 || lora_decode(data, len, &msg) != LORA_DEC_OK || msg.type != LORA_MSG_IDENT) {
        return;
    }
    portENTER_CRITICAL(&ident_lock);
    memcpy(ident_sha, msg.u.ident.app_elf_sha256, sizeof(ident_sha));
    portEXIT_CRITICAL(&ident_lock);
    xSemaphoreGive(ident_done);
}

bool lora_ident(uint8_t out[LORA_IDENT_LEN], uint32_t timeout_ms) {
    if (lora_up()) {
        return false; // the LoRa task owns the callback now
    }
    if (!ident_done && !(ident_done = xSemaphoreCreateBinary())) {
        return false;
    }
    if (eh_host_peer_data_register(LORA_MSG_ID_TO_P4, on_ident, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "IDENT: peer data callback not registered");
        return false;
    }
    xSemaphoreTake(ident_done, 0); // a give from an earlier call
    uint8_t    req[LORA_MSG_MAX];
    lora_msg_t msg   = {.type = LORA_MSG_IDENT_REQ};
    size_t     len   = lora_encode(&msg, req, sizeof(req));
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        eh_host_peer_data_send(LORA_MSG_ID_TO_C6, req, len); // a refusal is retried like a lost answer
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(until - now) <= 0) {
            return false;
        }
        TickType_t wait = until - now < pdMS_TO_TICKS(IDENT_RETRY_MS) ? until - now : pdMS_TO_TICKS(IDENT_RETRY_MS);
        if (xSemaphoreTake(ident_done, wait) == pdTRUE) {
            portENTER_CRITICAL(&ident_lock);
            memcpy(out, ident_sha, LORA_IDENT_LEN);
            portEXIT_CRITICAL(&ident_lock);
            return true;
        }
    }
}
