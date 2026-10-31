#include "lora_radio.h"

#include "esp_log.h"
#include "lichen_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

extern "C" {
#include "lora.h"
#include "lora_airtime.h"
}

#include <math.h>
#include <string.h>

#define RX_QUEUE_DEPTH 8

typedef struct {
    uint8_t len;
    int16_t rssi_x10, snr_x10;
    uint8_t data[255];
} rx_item_t;

static QueueHandle_t rx_queue;
static LoraRadio    *instance;

// Runs in the LoRa task: never block it, a full queue drops the packet
static void on_packet(uint8_t const *data, size_t len, int16_t rssi_x10, int16_t snr_x10) {
    rx_item_t item;
    if (len > sizeof(item.data)) {
        return;
    }
    item.len      = (uint8_t)len;
    item.rssi_x10 = rssi_x10;
    item.snr_x10  = snr_x10;
    memcpy(item.data, data, len);
    if (xQueueSend(rx_queue, &item, 0) != pdTRUE && instance) {
        instance->dropped++;
    }
}

void LoraRadio::begin() {
    instance = this;
    rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(rx_item_t));
    lora_set_packet_callback(on_packet);
}

int LoraRadio::recvRaw(uint8_t *bytes, int sz) {
    rx_item_t item;
    if (!rx_queue || xQueueReceive(rx_queue, &item, 0) != pdTRUE || item.len > sz) {
        return 0;
    }
    memcpy(bytes, item.data, item.len);
    n_recv++;
    last_rssi = item.rssi_x10 / 10.0f;
    last_snr  = item.snr_x10 / 10.0f;
    return item.len;
}

uint32_t LoraRadio::getEstAirtimeFor(int len_bytes) {
    uint8_t  sf, cr;
    uint32_t bw;
    uint16_t preamble;
    lora_get_radio(&sf, &bw, &cr, &preamble);
    return lora_airtime_us(len_bytes, sf, bw, cr, preamble) / 1000;
}

float LoraRadio::packetScore(float snr, int packet_len) {
    uint8_t  sf, cr;
    uint32_t bw;
    uint16_t preamble;
    lora_get_radio(&sf, &bw, &cr, &preamble);
    return lora_packet_score(snr, sf, packet_len);
}

// MeshCore's prefs keep MHz and kHz as floats (869.618 isn't exact in a float): rounded to whole kHz and Hz
void LoraRadio::setParams(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr) {
    uint32_t freq_hz = (uint32_t)lround(freq_mhz * 1000.0) * 1000;
    uint32_t bw_hz   = (uint32_t)lround(bw_khz * 1000.0);
    if (!lora_set_radio(freq_hz, bw_hz, sf, cr)) {
        ESP_LOGE("mesh", "radio settings refused: %lu Hz, %lu Hz, SF%u, CR4/%u", (unsigned long)freq_hz,
                 (unsigned long)bw_hz, sf, cr);
    }
}

uint32_t LoraRadio::getRngSeed() {
    return lichen_random_u32();
}

bool LoraRadio::startSendRaw(uint8_t const *bytes, int len) {
    if (len <= 0) {
        return false;
    }
    if (!lora_send(bytes, (size_t)len, getEstAirtimeFor(len))) {
        send_refused++;
        return false;
    }
    sending   = true;
    completed = false;
    return true;
}

bool LoraRadio::isSendComplete() {
    int state = lora_send_state();
    if (state == 0) {
        return false;
    }
    completed = true;
    if (state > 0) {
        n_sent++;
    } else {
        send_failed++;
    }
    return true;
}

void LoraRadio::onSendFinished() {
    if (sending && !completed) {
        send_timeouts++;
    }
    sending = false;
}

// MeshCore's prefs allow up to MAX_LORA_TX_POWER (22, the SX1262's high-power PA and MeshCore's badge variant)
void LoraRadio::setTxPower(int8_t dbm) {
    if (!lora_set_tx_power(dbm > 22 ? 22 : dbm)) {
        ESP_LOGE("mesh", "TX power %d dBm refused", dbm);
    }
}
