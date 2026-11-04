// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
/*
 * LoRa radio service for the P4 (#51, #65).
 *
 * The P4 drives it over ESP-Hosted peer data (lora_proto.h): CONFIG, RX_START, RX_STOP, STATUS_REQ, SEND, IDENT_REQ,
 * SWEEP_REQ. One task owns the SX1262 through RadioLib; the peer-data callback only queues the command and the DIO1
 * interrupt only wakes the task. A SEND interrupts receiving for one packet, is answered by a RESULT once it left the
 * radio (or failed), and receiving starts again. A SWEEP_REQ (#161) interrupts receiving for one pass over the band,
 * receive only, and receiving starts again on the channel.
 */

#include "lora_service.h"

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "eh_cp_feat_peer_data.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lora_irq.h"
#include "lora_proto.h"

#include <atomic>
#include <RadioLib.h>
#include <string.h>

static char const *TAG = "lora";

static_assert(LORA_IRQ_FLAG_RX_DONE == RADIOLIB_SX126X_IRQ_RX_DONE, "SX1262 IRQ bit");
static_assert(LORA_IRQ_FLAG_HEADER_ERR == RADIOLIB_SX126X_IRQ_HEADER_ERR, "SX1262 IRQ bit");

// RA-01SH wiring on the C6 (BUSY and DIO1 are bodge wires, #49); RXEN is not routed
#define PIN_RESET 1
#define PIN_MISO  2
#define PIN_TXEN  3
#define PIN_NSS   4
#define PIN_DIO1  5
#define PIN_SCK   6
#define PIN_MOSI  7
#define PIN_BUSY  11

// MeshCore's settings for this board (variants/why2025_badge: SX126X_CURRENT_LIMIT)
#define CURRENT_LIMIT_MA 140
// A transmission not done by its airtime plus this is given up (a missed DIO1)
#define TX_SLACK_US (1000 * 1000LL)

// RSSI sweep (#161): after entering RX on a new frequency the SX1262 needs about a millisecond before its instantaneous
// RSSI means anything (2 ticks at 1 kHz: at least one full millisecond), then the readings are spread a little
#define SWEEP_SETTLE_TICKS 2
#define SWEEP_SAMPLE_US    100

#define CMD_QUEUE_DEPTH 4
#define HEARTBEAT_US    (60 * 1000000LL)

typedef struct {
    uint16_t len;
    uint8_t  buf[LORA_MSG_MAX];
} lora_cmd_t;

static EspHal hal(PIN_SCK, PIN_MISO, PIN_MOSI, SPI2_HOST, 2000000);
static Module module(&hal, PIN_NSS, PIN_DIO1, PIN_RESET, PIN_BUSY);
static SX1262 radio(&module);

static QueueHandle_t cmd_queue;
static TaskHandle_t  lora_task_handle;

static lora_config_t         config;
static bool                  have_config;
static lora_status_t         status;           // only touched by the LoRa task, apart from the callback's dropped count
static std::atomic<uint32_t> callback_dropped; // written by the peer-data callback
static int64_t               tx_deadline_us;   // while LORA_STATE_TX

static IRAM_ATTR void on_dio1(void) {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(lora_task_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

// Runs in the ESP-Hosted RPC context: copy and get out
static void on_peer_data(uint32_t msg_id, uint8_t const *data, size_t len, void *ctx) {
    lora_cmd_t cmd;
    if (len > sizeof(cmd.buf)) {
        callback_dropped++;
        return;
    }
    cmd.len = (uint16_t)len;
    memcpy(cmd.buf, data, len);
    if (xQueueSend(cmd_queue, &cmd, 0) != pdTRUE) {
        callback_dropped++;
        return;
    }
    xTaskNotifyGive(lora_task_handle);
}

static void send_msg(lora_msg_t const *msg) {
    uint8_t buf[LORA_MSG_MAX];
    size_t  len = lora_encode(msg, buf, sizeof(buf));
    if (!len || eh_cp_feat_peer_data_send(LORA_MSG_ID_TO_P4, buf, len) != ESP_OK) {
        status.dropped++;
    }
}

static void send_result(uint8_t for_type, lora_result_code_t code, int16_t radio_err) {
    lora_msg_t msg         = {};
    msg.type               = LORA_MSG_RESULT;
    msg.u.result.for_type  = for_type;
    msg.u.result.code      = code;
    msg.u.result.radio_err = radio_err;
    send_msg(&msg);
}

static int16_t arm_receive(void) {
    // Header errors wake us as well, so they can be counted
    return radio.startReceive(
        RADIOLIB_SX126X_RX_TIMEOUT_INF,
        RADIOLIB_IRQ_RX_DEFAULT_FLAGS,
        (1UL << RADIOLIB_IRQ_RX_DONE) | (1UL << RADIOLIB_IRQ_HEADER_ERR),
        0
    );
}

static int16_t start_rx(void) {
    int16_t err = radio.begin(
        config.freq_hz / 1e6f,
        config.bw_hz / 1e3f,
        config.sf,
        config.cr,
        config.sync_word,
        config.tx_power_dbm,
        config.preamble,
        0.0f, // crystal, no TCXO
        false
    );
    if (err == RADIOLIB_ERR_NONE) {
        // MeshCore's settings for this board: TXEN on GPIO3 (driven by RadioLib only while transmitting), no RXEN
        radio.setRfSwitchPins(RADIOLIB_NC, PIN_TXEN);
        err = radio.setDio2AsRfSwitch(false);
    }
    if (err == RADIOLIB_ERR_NONE) {
        err = radio.setCurrentLimit(CURRENT_LIMIT_MA);
    }
    if (err == RADIOLIB_ERR_NONE) {
        err = radio.setRxBoostedGainMode(true);
    }
    if (err == RADIOLIB_ERR_NONE) {
        radio.setPacketReceivedAction(on_dio1);
        err = arm_receive();
    }
    status.last_radio_err = err;
    status.state          = err == RADIOLIB_ERR_NONE ? LORA_STATE_RX : LORA_STATE_ERROR;
    if (err != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "radio start failed: %d", err);
    } else {
        ESP_LOGI(
            TAG,
            "receiving on %.3f MHz, %.1f kHz, SF%u, CR%u",
            config.freq_hz / 1e6,
            config.bw_hz / 1e3,
            config.sf,
            config.cr
        );
    }
    return err;
}

// Evidence for a C6 that stops answering (#56): uptime shows a restart, heap and stack show exhaustion
static void fill_health(lora_status_t *st) {
    st->uptime_s   = (uint32_t)(esp_timer_get_time() / 1000000);
    st->heap_free  = esp_get_free_heap_size();
    st->heap_min   = esp_get_minimum_free_heap_size();
    st->stack_free = (uint16_t)uxTaskGetStackHighWaterMark(NULL);
}

// After a transmission (done or not): back to receiving
static void finish_tx(void) {
    radio.finishTransmit();
    radio.setPacketReceivedAction(on_dio1);
    int16_t err = arm_receive();
    status.last_radio_err = err;
    status.state          = err == RADIOLIB_ERR_NONE ? LORA_STATE_RX : LORA_STATE_ERROR;
    if (err != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "receive after transmit failed: %d", err);
    }
}

static void handle_tx_event(bool timed_out) {
    bool sent = !timed_out && (radio.getIrqFlags() & RADIOLIB_SX126X_IRQ_TX_DONE) != 0;
    send_result(LORA_MSG_SEND, sent ? LORA_RESULT_OK : LORA_RESULT_RADIO_ERROR, 0);
    if (!sent) {
        ESP_LOGW(TAG, "tx %s", timed_out ? "timed out" : "ended without TX_DONE");
    }
    finish_tx();
}

// The average of samples instantaneous readings in the receive mode the radio is in, dBm * 10
static int16_t sample_rssi_x10(uint8_t samples) {
    int32_t sum = 0;
    for (uint8_t i = 0; i < samples; i++) {
        esp_rom_delay_us(SWEEP_SAMPLE_US);
        sum += (int32_t)(radio.getRSSI(false) * 10);
    }
    return (int16_t)(sum / samples);
}

// One step of a sweep: frequencies are only set in standby, and RSSI is only read while receiving
static int16_t sweep_step(uint32_t freq_hz, uint8_t samples, int16_t *rssi_x10) {
    int16_t err = radio.standby();
    if (err == RADIOLIB_ERR_NONE) {
        err = radio.setFrequency(freq_hz / 1e6f);
    }
    if (err == RADIOLIB_ERR_NONE) {
        err = radio.startReceive();
    }
    if (err == RADIOLIB_ERR_NONE) {
        vTaskDelay(SWEEP_SETTLE_TICKS);
        *rssi_x10 = sample_rssi_x10(samples);
    }
    return err;
}

// Back to receiving on the channel as before the sweep. The modem settings were never touched, so the frequency and a
// new receive are enough; if that fails, the full start CONFIG uses.
static int16_t sweep_restore(void) {
    int16_t err = radio.standby();
    if (err == RADIOLIB_ERR_NONE) {
        err = radio.setFrequency(config.freq_hz / 1e6f);
    }
    if (err == RADIOLIB_ERR_NONE) {
        radio.setPacketReceivedAction(on_dio1);
        err = arm_receive(); // clears the IRQ flags a packet heard during the sweep left behind
    }
    if (err != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "back to the channel after the sweep failed (%d): starting the radio again", err);
        err = start_rx();
    }
    status.last_radio_err = err;
    status.state          = err == RADIOLIB_ERR_NONE ? LORA_STATE_RX : LORA_STATE_ERROR;
    return err;
}

// RSSI sweep (#161): receive only, never transmits. Only from receiving (not while a packet goes out, not while the P4
// has the radio switched off), and receiving again afterwards. Answered by SWEEP, or by a RESULT when refused or failed.
static void handle_sweep(lora_sweep_req_t const *req) {
    if (status.state != LORA_STATE_RX) {
        send_result(LORA_MSG_SWEEP_REQ, LORA_RESULT_BAD_STATE, 0);
        return;
    }
    if (!lora_sweep_req_valid(req)) {
        send_result(LORA_MSG_SWEEP_REQ, LORA_RESULT_BAD_PARAM, 0);
        return;
    }
    static lora_msg_t reply; // 270 bytes: off the task's stack
    memset(&reply, 0, sizeof(reply));
    reply.type              = LORA_MSG_SWEEP;
    lora_sweep_t *sw        = &reply.u.sweep;
    sw->start_hz            = req->start_hz;
    sw->step_hz             = req->step_hz;
    sw->seq                 = req->seq;
    sw->count               = req->count;
    int16_t err             = RADIOLIB_ERR_NONE;
    for (uint8_t i = 0; i < req->count && err == RADIOLIB_ERR_NONE; i++) {
        err = sweep_step(req->start_hz + req->step_hz * i, req->samples, &sw->rssi_x10[i]);
    }
    int16_t back = sweep_restore();
    if (err != RADIOLIB_ERR_NONE || back != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "sweep failed: %d, back to the channel: %d", err, back);
        send_result(LORA_MSG_SWEEP_REQ, LORA_RESULT_RADIO_ERROR, err != RADIOLIB_ERR_NONE ? err : back);
        return;
    }
    vTaskDelay(SWEEP_SETTLE_TICKS);
    sw->channel_rssi_x10 = sample_rssi_x10(req->samples);
    send_msg(&reply);
}

static void handle_command(lora_cmd_t const *cmd) {
    lora_msg_t msg;
    if (lora_decode(cmd->buf, cmd->len, &msg) != LORA_DEC_OK) {
        status.ignored++;
        return;
    }

    switch (msg.type) {
        case LORA_MSG_CONFIG:
            if (!lora_config_valid(&msg.u.config)) {
                send_result(msg.type, LORA_RESULT_BAD_PARAM, 0);
                return;
            }
            config      = msg.u.config;
            have_config = true;
            if (status.state == LORA_STATE_TX) {
                send_result(LORA_MSG_SEND, LORA_RESULT_RADIO_ERROR, 0); // cut short by the new settings
            }
            if (status.state == LORA_STATE_UNCONFIGURED) {
                status.state = LORA_STATE_IDLE;
            } else if ((status.state == LORA_STATE_RX || status.state == LORA_STATE_TX) &&
                       start_rx() != RADIOLIB_ERR_NONE) {
                send_result(msg.type, LORA_RESULT_RADIO_ERROR, status.last_radio_err);
                return;
            }
            send_result(msg.type, LORA_RESULT_OK, 0);
            return;
        case LORA_MSG_RX_START:
            if (!have_config) {
                send_result(msg.type, LORA_RESULT_BAD_STATE, 0);
            } else if (start_rx() != RADIOLIB_ERR_NONE) {
                send_result(msg.type, LORA_RESULT_RADIO_ERROR, status.last_radio_err);
            } else {
                send_result(msg.type, LORA_RESULT_OK, 0);
            }
            return;
        case LORA_MSG_RX_STOP:
            if (status.state == LORA_STATE_RX) {
                radio.standby();
                status.state = LORA_STATE_IDLE;
            }
            send_result(msg.type, LORA_RESULT_OK, 0);
            return;
        case LORA_MSG_STATUS_REQ: {
            lora_msg_t reply        = {};
            reply.type              = LORA_MSG_STATUS;
            reply.u.status          = status;
            reply.u.status.dropped += callback_dropped.load();
            fill_health(&reply.u.status);
            if (status.state == LORA_STATE_RX) {
                reply.u.status.rssi_x10 = (int16_t)(radio.getRSSI(false) * 10);
            }
            send_msg(&reply);
            return;
        }
        case LORA_MSG_IDENT_REQ: {
            // The running app's, not the flash's: the P4 compares it with the bundled .bin before trusting a warm boot
            // (#36). Answered whatever the radio's state.
            lora_msg_t reply = {};
            reply.type       = LORA_MSG_IDENT;
            static_assert(sizeof(reply.u.ident.app_elf_sha256) == sizeof(esp_app_get_description()->app_elf_sha256),
                          "IDENT sha size");
            memcpy(reply.u.ident.app_elf_sha256, esp_app_get_description()->app_elf_sha256, LORA_IDENT_SHA_LEN);
            send_msg(&reply);
            return;
        }
        case LORA_MSG_SEND: {
            if (status.state != LORA_STATE_RX) {
                send_result(msg.type, LORA_RESULT_BAD_STATE, 0);
                return;
            }
            radio.setPacketSentAction(on_dio1);
            int16_t err = radio.startTransmit(msg.u.tx.data, msg.u.tx.len);
            if (err != RADIOLIB_ERR_NONE) {
                status.last_radio_err = err;
                send_result(msg.type, LORA_RESULT_RADIO_ERROR, err);
                finish_tx();
                return;
            }
            status.state   = LORA_STATE_TX;
            tx_deadline_us = esp_timer_get_time() + (int64_t)radio.getTimeOnAir(msg.u.tx.len) + TX_SLACK_US;
            ESP_LOGI(TAG, "tx %u B", (unsigned)msg.u.tx.len);
            return;
        }
        case LORA_MSG_SWEEP_REQ: handle_sweep(&msg.u.sweep_req); return;
        default: status.ignored++; return; // a C6 -> P4 type sent our way
    }
}

static void handle_radio_event(void) {
    lora_irq_event_t event = lora_irq_classify(radio.getIrqFlags());
    if (event == LORA_IRQ_HEADER_ERROR) {
        status.header_errors++;
    } else if (event == LORA_IRQ_PACKET) {
        lora_msg_t msg = {};
        msg.type       = LORA_MSG_RX_PACKET;
        size_t len     = radio.getPacketLength();
        if (len == 0 || len > sizeof(msg.u.rx.data)) {
            status.header_errors++;
        } else {
            int16_t err = radio.readData(msg.u.rx.data, len);
            if (err == RADIOLIB_ERR_CRC_MISMATCH) {
                status.crc_errors++;
            } else if (err != RADIOLIB_ERR_NONE) {
                status.last_radio_err = err;
            } else {
                msg.u.rx.len      = (uint8_t)len;
                msg.u.rx.rssi_x10 = (int16_t)(radio.getRSSI() * 10);
                msg.u.rx.snr_x10  = (int16_t)(radio.getSNR() * 10);
                msg.u.rx.c6_ms    = esp_log_timestamp();
                status.packets++;
                ESP_LOGI(
                    TAG,
                    "rx %u B rssi %.1f snr %.1f",
                    (unsigned)len,
                    msg.u.rx.rssi_x10 / 10.0,
                    msg.u.rx.snr_x10 / 10.0
                );
                send_msg(&msg);
            }
        }
    }
    int16_t err = arm_receive();
    if (err != RADIOLIB_ERR_NONE) {
        status.last_radio_err = err;
        status.state          = LORA_STATE_ERROR;
        ESP_LOGE(TAG, "re-arming receive failed: %d", err);
    }
}

// One line on the C6 console a minute, read with "badgevms c6log": shows the C6 is alive even when the P4 cannot
// reach it (#56)
static void log_heartbeat(void) {
    lora_status_t h = {};
    fill_health(&h);
    ESP_LOGI(
        TAG,
        "alive up %u s heap %u/%u stack %u state %u packets %u dropped %u",
        (unsigned)h.uptime_s,
        (unsigned)h.heap_free,
        (unsigned)h.heap_min,
        (unsigned)h.stack_free,
        (unsigned)status.state,
        (unsigned)status.packets,
        (unsigned)(status.dropped + callback_dropped.load())
    );
}

static void lora_task(void *arg) {
    int64_t next_heartbeat_us = 0;
    for (;;) {
        if (esp_timer_get_time() >= next_heartbeat_us) {
            log_heartbeat();
            next_heartbeat_us = esp_timer_get_time() + HEARTBEAT_US;
        }
        // Woken by a command or by DIO1; the timeout re-checks DIO1 in case a wake-up was missed
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        lora_cmd_t cmd;
        while (xQueueReceive(cmd_queue, &cmd, 0) == pdTRUE) {
            handle_command(&cmd);
        }
        // DIO1 stays high until the IRQ flags are cleared
        if (status.state == LORA_STATE_RX && gpio_get_level((gpio_num_t)PIN_DIO1)) {
            handle_radio_event();
        } else if (status.state == LORA_STATE_TX && gpio_get_level((gpio_num_t)PIN_DIO1)) {
            handle_tx_event(false);
        } else if (status.state == LORA_STATE_TX && esp_timer_get_time() >= tx_deadline_us) {
            handle_tx_event(true);
        }
    }
}

bool lora_service_start(void) {
    cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(lora_cmd_t));
    if (!cmd_queue) {
        ESP_LOGE(TAG, "no memory for the command queue");
        return false;
    }
    // Below the ESP-Hosted transport tasks so Wi-Fi and BLE traffic come first
    if (xTaskCreate(lora_task, "lora", 4096, NULL, 2, &lora_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "task not started");
        return false;
    }
    if (eh_cp_feat_peer_data_register_callback(LORA_MSG_ID_TO_C6, on_peer_data, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "peer data callback not registered");
        return false;
    }
    return true;
}
