// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
/*
 * LoRa radio service for the P4 (#51, #65).
 *
 * The P4 drives it over ESP-Hosted peer data (lora_proto.h): CONFIG, RX_START, RX_STOP, STATUS_REQ, SEND, IDENT_REQ,
 * SWEEP_REQ. One task owns the SX1262 through RadioLib; the peer-data callback only queues the command and the DIO1
 * interrupt only wakes the task. A SEND interrupts receiving for one packet, is answered by a RESULT once it left the
 * radio (or failed), and receiving starts again. A SWEEP_REQ (#161) interrupts receiving for one pass over the band,
 * receive only, and receiving starts again on the channel. REG_GET / REG_SET (#309) are served from the C6's settings
 * registry (reg_c6.c) on this task as well, so its LoRa getters see the radio between commands, never halfway through.
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
#include "lora_readback.h"
#include "lora_tune.h"
#include "reg_c6.h"

#include <atomic>
#include <RadioLib.h>
#include <string.h>

static char const *TAG = "lora";

static_assert(LORA_IRQ_FLAG_RX_DONE == RADIOLIB_SX126X_IRQ_RX_DONE, "SX1262 IRQ bit");
static_assert(LORA_IRQ_FLAG_HEADER_ERR == RADIOLIB_SX126X_IRQ_HEADER_ERR, "SX1262 IRQ bit");
// lora_rb_bw_hz's table (lora_readback.c) against RadioLib's codes
static_assert(RADIOLIB_SX126X_LORA_BW_7_8 == 0x00 && RADIOLIB_SX126X_LORA_BW_10_4 == 0x08 &&
                  RADIOLIB_SX126X_LORA_BW_15_6 == 0x01 && RADIOLIB_SX126X_LORA_BW_20_8 == 0x09 &&
                  RADIOLIB_SX126X_LORA_BW_31_25 == 0x02 && RADIOLIB_SX126X_LORA_BW_41_7 == 0x0A &&
                  RADIOLIB_SX126X_LORA_BW_62_5 == 0x03 && RADIOLIB_SX126X_LORA_BW_125_0 == 0x04 &&
                  RADIOLIB_SX126X_LORA_BW_250_0 == 0x05 && RADIOLIB_SX126X_LORA_BW_500_0 == 0x06,
              "lora_rb_bw_hz codes");

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
static bool                  radio_started;    // the last start (begin and the board settings) succeeded
static int8_t                applied_power_dbm; // the power of that start
static uint32_t              applied_frf;       // the SetRfFrequency steps tune() wrote last, 0 for none
static uint32_t              reg_served;        // REG_GET / REG_SET answered, for the heartbeat

// RadioLib 8 keeps the modem settings it gave the chip in private members without getters, and the SX126x has no
// command that reads them back. These read them, and reach the protected frequency and packet status commands, without
// patching the library or opening every class to everyone (RADIOLIB_GODMODE): an explicit instantiation may name
// private members ([temp.spec.general]/6). A RadioLib update that renames or retypes one fails to compile here. What
// the members mean (codingRate = cr - 4, bandwidth a chip code) and the behaviour lora_tune.h and
// lora_rb_signal_rssi_x10 make up for are only checked by reading 8.0.0's source (SX126x_config.cpp, SX126x.cpp,
// SX1262.cpp): a new pin in c6/main/idf_component.yml has to re-read them and then move this assert.
static_assert(RADIOLIB_VERSION_MAJOR == 8 && RADIOLIB_VERSION_MINOR == 0 && RADIOLIB_VERSION_PATCH == 0,
              "RADIOLIB_PEEK reads 8.0.0's private members: re-check their meaning before moving the radiolib pin");
template <typename Tag, typename Tag::type Member> struct radiolib_peek {
    friend typename Tag::type peek(Tag) { return Member; }
};
#define RADIOLIB_PEEK(tag, member_type, member)                                                                        \
    struct tag {                                                                                                       \
        using type = member_type;                                                                                      \
        friend type peek(tag);                                                                                         \
    };                                                                                                                 \
    template struct radiolib_peek<tag, &SX126x::member>
RADIOLIB_PEEK(peek_set_frf, int16_t (SX126x::*)(uint32_t), setRfFrequency);
RADIOLIB_PEEK(peek_pkt_status, uint32_t (SX126x::*)(), getPacketStatus);
RADIOLIB_PEEK(peek_bw, uint8_t SX126x::*, bandwidth);
RADIOLIB_PEEK(peek_sf, uint8_t SX126x::*, spreadingFactor);
RADIOLIB_PEEK(peek_cr, uint8_t SX126x::*, codingRate);
RADIOLIB_PEEK(peek_preamble, uint16_t SX126x::*, preambleLengthLoRa);
RADIOLIB_PEEK(peek_read_reg, int16_t (SX126x::*)(uint16_t, uint8_t *, uint8_t), readRegister);

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

// After RadioLib set freq_hz (image calibration, its bookkeeping): the steps RadioLib 7.8.1 gave the chip, so the
// carrier stays where it was (lora_tune.h). In standby, never while transmitting.
static int16_t tune(uint32_t freq_hz) {
    uint32_t const frf = lora_tune_frf(freq_hz);
    int16_t const  err = (radio.*peek(peek_set_frf{}))(frf);
    applied_frf        = err == RADIOLIB_ERR_NONE ? frf : 0;
    return err;
}

static int16_t set_frequency(uint32_t freq_hz) {
    int16_t err = radio.setFrequency(freq_hz);
    return err == RADIOLIB_ERR_NONE ? tune(freq_hz) : err;
}

static int16_t begin_radio(void) {
    lora_tune_t t;
    lora_tune(&config, &t);
    ConfigLoRa_t cfg;
    cfg.frequency       = t.freq_hz;
    cfg.bandwidth       = t.bw_hz;
    cfg.spreadingFactor = t.sf;
    cfg.codingRate      = t.cr;
    cfg.syncWord        = t.sync_word;
    cfg.power           = t.power_dbm;
    cfg.preambleLength  = t.preamble;
    // Crystal, no TCXO, DC-DC regulator, as 7.8.1's begin(..., 0.0f, false) had it: RadioLib 8 defaults to a 1.6 V
    // TCXO and only falls back to the crystal after the oscillator fails to start
    radio.tcxoVoltage     = RadioLibTCXOVoltage_t::VoltageNone;
    radio.useRegulatorLDO = false;
    int16_t err           = radio.begin(cfg);
    return err == RADIOLIB_ERR_NONE ? tune(t.freq_hz) : err;
}

static int16_t start_rx(void) {
    int16_t err = begin_radio();
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
        radio.setIrqAction(on_dio1);
        err = arm_receive();
    }
    status.last_radio_err = err;
    status.state          = err == RADIOLIB_ERR_NONE ? LORA_STATE_RX : LORA_STATE_ERROR;
    radio_started         = err == RADIOLIB_ERR_NONE;
    applied_power_dbm     = radio_started ? config.tx_power_dbm : 0;
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
    radio.setIrqAction(on_dio1);
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
        err = set_frequency(freq_hz);
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
        err = set_frequency(config.freq_hz);
    }
    if (err == RADIOLIB_ERR_NONE) {
        radio.setIrqAction(on_dio1);
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

// Fetches only; lora_rb_decode decides what the values mean and whether they are complete
bool lora_service_readback(lora_config_t *out) {
    lora_rb_raw_t raw = {};
    raw.started       = radio_started;
    if (radio_started) { // no SPI to a radio that was never started
        uint8_t sync[2];
        raw.regs_read = (radio.*peek(peek_read_reg{}))(RADIOLIB_SX126X_REG_LORA_SYNC_WORD_MSB, sync, sizeof(sync)) ==
                        RADIOLIB_ERR_NONE;
        raw.sync_msb  = sync[0];
        raw.sync_lsb  = sync[1];
        raw.frf       = applied_frf;
        raw.bw_code   = radio.*peek(peek_bw{});
        raw.sf        = radio.*peek(peek_sf{});
        raw.cr_code   = radio.*peek(peek_cr{});
        raw.preamble  = radio.*peek(peek_preamble{});
        raw.power_dbm = applied_power_dbm;
    }
    return lora_rb_decode(&raw, out);
}

uint8_t lora_service_state(void) {
    return status.state;
}

// A REG_GET (set NULL) or REG_SET from the P4: always one REG_VAL back, an unknown id with UNKNOWN (#307)
static void handle_reg(uint16_t id, uint8_t seq, uint32_t const *set) {
    reg_reply_t  r;
    reg_status_t st = reg_serve(reg_c6_table, reg_c6_count, id, set, &r);
    static lora_msg_t reply; // 300 bytes: off the task's stack, as in handle_sweep (LoRa task only)
    memset(&reply, 0, sizeof(reply));
    reply.type      = LORA_MSG_REG_VAL;
    reply.u.reg_val = {id, seq, (uint8_t)st, r.type, r.value};
    send_msg(&reply);
    reg_served++;
    // Sets are rare and worth a line; the P4 may read keys often, those only count in the heartbeat
    if (set) {
        ESP_LOGI("reg", "set 0x%04x = %lu: status %u, applied %lu", (unsigned)id, (unsigned long)*set, (unsigned)st,
                 (unsigned long)r.value);
    } else {
        ESP_LOGD("reg", "get 0x%04x: status %u, value %lu", (unsigned)id, (unsigned)st, (unsigned long)r.value);
    }
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
            radio.setIrqAction(on_dio1);
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
        case LORA_MSG_REG_GET: handle_reg(msg.u.reg_get.id, msg.u.reg_get.seq, NULL); return;
        case LORA_MSG_REG_SET: handle_reg(msg.u.reg_set.id, msg.u.reg_set.seq, &msg.u.reg_set.value); return;
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
                msg.u.rx.rssi_x10 = lora_rb_signal_rssi_x10((radio.*peek(peek_pkt_status{}))());
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
        "alive up %u s heap %u/%u stack %u state %u packets %u dropped %u reg %u",
        (unsigned)h.uptime_s,
        (unsigned)h.heap_free,
        (unsigned)h.heap_min,
        (unsigned)h.stack_free,
        (unsigned)status.state,
        (unsigned)status.packets,
        (unsigned)(status.dropped + callback_dropped.load()),
        (unsigned)reg_served
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
