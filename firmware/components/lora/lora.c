/*
 * LoRa receive bridge, P4 side (#51), BadgeVMS's driver adapted for the lean firmware (#61).
 *
 * The C6 owns the SX1262 and speaks lora_proto.h over ESP-Hosted peer data. Here one task sends it the radio config,
 * polls its status (lora_link.c decides when) and logs the packets it pushes. The peer-data callback only queues;
 * every send happens in this task, since a send blocks until the C6 acks it.
 */

#include "lora.h"

#include "fw_console.h"
#include "eh_host_feat_peer_data.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lora_link.h"
#include "lora_proto.h"
#include "lora_tx.h"

#include <stdio.h>
#include <stdlib.h>

#include <string.h>

static char const TAG[] = "lora";

#define LORA_TOML       "/storage/lora.toml"
#define LORA_TOML_MAX   2048
#define RX_QUEUE_DEPTH  8
#define STATUS_LINE_MAX 256

typedef struct {
    uint16_t len;
    uint8_t  buf[LORA_MSG_MAX];
} lora_rx_msg_t;

static QueueHandle_t rx_queue;
static lora_config_t config = LORA_CONFIG_DEFAULT;

// Shared with the console task
static portMUX_TYPE  state_lock = portMUX_INITIALIZER_UNLOCKED;
static lora_status_t c6_status;
static bool          have_status;
static uint32_t      status_at_ms;
static uint32_t      p4_dropped;
static uint32_t      p4_bad;
static uint32_t      packets;
static int16_t       last_rssi_x10;
static char          last_summary[LORA_SUMMARY_MAX];

static void cmd_lora(char const *args);

// "fw lora raw on": also print each received packet as hex, for test vectors (#61), and every send's answer time
static bool log_raw;

static lora_packet_cb_t    packet_cb;
static char const         *owner;  // who claimed the radio (#84 T7)
static lora_rx_formatter_t rx_fmt;
static bool             stop_requested; // lora_stop -> the task
static bool             stopped;        // the task -> everyone
static bool             started;        // lora_start succeeded

bool lora_claim(char const *who, lora_packet_cb_t cb) {
    if (owner && strcmp(owner, who) != 0) {
        ESP_LOGE(TAG, "radio claimed by %s: %s refused", owner, who);
        return false;
    }
    owner     = who;
    packet_cb = cb;
    return true;
}

void lora_release(char const *who) {
    if (owner && strcmp(owner, who) == 0) {
        packet_cb = NULL;
        rx_fmt    = NULL;
        owner     = NULL;
    }
}

void lora_set_rx_formatter(lora_rx_formatter_t fmt) {
    rx_fmt = fmt;
}

static uint32_t now_ms(void);
static bool     send_to_c6(lora_msg_t const *msg);

// The packet on its way out through the C6 (one at a time), under state_lock
static lora_tx_t tx;
static uint32_t  tx_sent_ms, tx_airtime_ms; // the current send, for the round-trip log

// Set by lora_set_radio(), sent to the C6 by the LoRa task
static bool          config_changed;
static lora_config_t boot_config = LORA_CONFIG_DEFAULT;

bool lora_set_tx_power(int8_t dbm) {
    portENTER_CRITICAL(&state_lock);
    lora_config_t c = config;
    c.tx_power_dbm  = dbm;
    bool ok         = lora_config_valid(&c);
    if (ok && c.tx_power_dbm != config.tx_power_dbm) {
        config         = c;
        config_changed = true;
    }
    portEXIT_CRITICAL(&state_lock);
    return ok;
}

bool lora_send(uint8_t const *bytes, size_t len, uint32_t airtime_ms) {
    if (len == 0 || len > sizeof(((lora_tx_packet_t *)0)->data)) {
        return false;
    }
    if (__atomic_load_n(&stopped, __ATOMIC_ACQUIRE)) {
        return false;
    }
    portENTER_CRITICAL(&state_lock);
    bool ok = lora_tx_begin(&tx, now_ms(), airtime_ms);
    if (ok) {
        tx_sent_ms    = now_ms();
        tx_airtime_ms   = airtime_ms;
    }
    portEXIT_CRITICAL(&state_lock);
    if (!ok) {
        return false;
    }
    lora_msg_t msg = {.type = LORA_MSG_SEND};
    msg.u.tx.len   = (uint8_t)len;
    memcpy(msg.u.tx.data, bytes, len);
    if (!send_to_c6(&msg)) {
        portENTER_CRITICAL(&state_lock);
        lora_tx_result(&tx, LORA_RESULT_RADIO_ERROR);
        portEXIT_CRITICAL(&state_lock);
    }
    return true;
}

int lora_send_state(void) {
    portENTER_CRITICAL(&state_lock);
    lora_tx_state_t st = lora_tx_poll(&tx, now_ms());
    lora_tx_finish(&tx);
    portEXIT_CRITICAL(&state_lock);
    return st == LORA_TX_BUSY ? 0 : st == LORA_TX_DONE ? 1 : -1;
}

void lora_get_boot_radio(uint32_t *freq_hz, uint32_t *bw_hz, uint8_t *sf, uint8_t *cr) {
    *freq_hz = boot_config.freq_hz;
    *bw_hz   = boot_config.bw_hz;
    *sf      = boot_config.sf;
    *cr      = boot_config.cr;
}

void lora_get_radio(uint8_t *sf, uint32_t *bw_hz, uint8_t *cr, uint16_t *preamble) {
    portENTER_CRITICAL(&state_lock);
    *sf       = config.sf;
    *bw_hz    = config.bw_hz;
    *cr       = config.cr;
    *preamble = config.preamble;
    portEXIT_CRITICAL(&state_lock);
}

bool lora_set_radio(uint32_t freq_hz, uint32_t bw_hz, uint8_t sf, uint8_t cr) {
    portENTER_CRITICAL(&state_lock);
    lora_config_t c = config;
    c.freq_hz       = freq_hz;
    c.bw_hz         = bw_hz;
    c.sf            = sf;
    c.cr            = cr;
    bool ok         = lora_config_valid(&c);
    if (ok) {
        config         = c;
        config_changed = true;
    }
    portEXIT_CRITICAL(&state_lock);
    return ok;
}

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void load_config(void) {
    FILE *fp = fopen(LORA_TOML, "r");
    if (!fp) {
        ESP_LOGW(TAG, "no %s, using the default preset", LORA_TOML);
        return;
    }
    char *text = malloc(LORA_TOML_MAX);
    if (!text) {
        fclose(fp);
        ESP_LOGE(TAG, "no memory to read %s, using the default preset", LORA_TOML);
        return;
    }
    size_t len = fread(text, 1, LORA_TOML_MAX - 1, fp);
    fclose(fp);
    text[len] = '\0';

    char err[128];
    if (!lora_cfg_parse(text, len, &config, err, sizeof(err))) {
        ESP_LOGE(TAG, "%s: %s; using the default preset", LORA_TOML, err);
    }
    free(text);
}

// Runs in the ESP-Hosted RX context: copy and get out
static void on_peer_data(uint32_t msg_id, uint8_t const *data, size_t len, void *ctx) {
    lora_rx_msg_t msg;
    if (__atomic_load_n(&stopped, __ATOMIC_ACQUIRE)) {
        return; // stopped for an update: nobody reads the queue
    }
    if (len > sizeof(msg.buf)) {
        portENTER_CRITICAL(&state_lock);
        p4_dropped++;
        portEXIT_CRITICAL(&state_lock);
        return;
    }
    msg.len = (uint16_t)len;
    memcpy(msg.buf, data, len);
    if (xQueueSend(rx_queue, &msg, 0) != pdTRUE) {
        portENTER_CRITICAL(&state_lock);
        p4_dropped++;
        portEXIT_CRITICAL(&state_lock);
    }
}

static bool send_to_c6(lora_msg_t const *msg) {
    static bool failing;
    uint8_t     buf[LORA_MSG_MAX];
    size_t      len = lora_encode(msg, buf, sizeof(buf));
    esp_err_t   err = len ? eh_host_peer_data_send(LORA_MSG_ID_TO_C6, buf, len) : ESP_ERR_INVALID_SIZE;
    if (err != ESP_OK && !failing) {
        ESP_LOGE(TAG, "cannot reach the C6: %s", esp_err_to_name(err));
    }
    failing = err != ESP_OK;
    if (failing) {
        // Until the C6 answers again, MeshCore's sends fail at once instead of blocking its loop on the RPC
        portENTER_CRITICAL(&state_lock);
        lora_tx_link(&tx, false);
        portEXIT_CRITICAL(&state_lock);
    }
    return err == ESP_OK;
}

static void run_actions(unsigned act) {
    if (act & LORA_ACT_SEND_CONFIG) {
        lora_msg_t msg = {.type = LORA_MSG_CONFIG};
        portENTER_CRITICAL(&state_lock);
        msg.u.config = config;
        portEXIT_CRITICAL(&state_lock);
        send_to_c6(&msg);
    }
    if (act & LORA_ACT_RX_START) {
        lora_msg_t msg = {.type = LORA_MSG_RX_START};
        send_to_c6(&msg);
    }
    if (act & LORA_ACT_STATUS_REQ) {
        lora_msg_t msg = {.type = LORA_MSG_STATUS_REQ};
        send_to_c6(&msg);
    }
}

static char const *msg_name(uint8_t type) {
    switch (type) {
        case LORA_MSG_CONFIG: return "config";
        case LORA_MSG_RX_START: return "rx start";
        case LORA_MSG_RX_STOP: return "rx stop";
        case LORA_MSG_STATUS_REQ: return "status";
        case LORA_MSG_SEND: return "send";
        default: return "?";
    }
}

static void log_packet(lora_rx_packet_t const *rx) {
    char summary[128];
    if (!rx_fmt) {
        snprintf(summary, sizeof(summary), "no owner");
    } else if (!rx_fmt(rx->data, rx->len, summary, sizeof(summary))) {
        snprintf(summary, sizeof(summary), "undecodable");
    }
    ESP_LOGW(TAG, "rx %u B rssi %.1f snr %.1f | %s", rx->len, rx->rssi_x10 / 10.0, rx->snr_x10 / 10.0, summary);
    if (packet_cb) {
        packet_cb(rx->data, rx->len, rx->rssi_x10, rx->snr_x10);
    }
    if (log_raw) {
        char hex[2 * sizeof(rx->data) + 1];
        for (size_t i = 0; i < rx->len; i++) {
            snprintf(hex + 2 * i, 3, "%02x", rx->data[i]);
        }
        hex[2 * rx->len] = '\0';
        printf("lora raw %s\n", hex);
    }
    portENTER_CRITICAL(&state_lock);
    packets++;
    last_rssi_x10 = rx->rssi_x10;
    snprintf(last_summary, sizeof(last_summary), "%s", summary);
    portEXIT_CRITICAL(&state_lock);
}

static void lora_task(void *arg) {
    lora_sup_t sup;
    lora_sup_init(&sup, now_ms());

    for (;;) {
        if (__atomic_load_n(&stop_requested, __ATOMIC_ACQUIRE)) {
            // The C6 stops listening (its own bridge keeps running for Wi-Fi), then this task ends. Sent from here so
            // the stop is not interleaved with this task's own sends; the mesh task's lora_send (its own send_to_c6)
            // is stopped first by main.c and refused from here on
            lora_msg_t stop = {.type = LORA_MSG_RX_STOP};
            send_to_c6(&stop);
            __atomic_store_n(&stopped, true, __ATOMIC_RELEASE);
            ESP_LOGW(TAG, "LoRa stopped for the update");
            vTaskDelete(NULL);
        }
        run_actions(lora_sup_tick(&sup, now_ms()));
        portENTER_CRITICAL(&state_lock);
        bool changed   = config_changed;
        config_changed = false;
        portEXIT_CRITICAL(&state_lock);
        if (changed) {
            ESP_LOGW(TAG, "new radio settings from MeshCore");
            run_actions(LORA_ACT_SEND_CONFIG | LORA_ACT_RX_START);
        }

        lora_rx_msg_t raw;
        if (xQueueReceive(rx_queue, &raw, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        lora_msg_t msg;
        if (lora_decode(raw.buf, raw.len, &msg) != LORA_DEC_OK) {
            portENTER_CRITICAL(&state_lock);
            p4_bad++;
            portEXIT_CRITICAL(&state_lock);
            continue;
        }

        portENTER_CRITICAL(&state_lock);
        lora_tx_link(&tx, true); // the C6 spoke: sends may go to it again
        portEXIT_CRITICAL(&state_lock);
        switch (msg.type) {
            case LORA_MSG_RX_PACKET: log_packet(&msg.u.rx); break;
            case LORA_MSG_STATUS:
                portENTER_CRITICAL(&state_lock);
                c6_status    = msg.u.status;
                have_status  = true;
                status_at_ms = now_ms();
                portEXIT_CRITICAL(&state_lock);
                if (msg.u.status.state == LORA_STATE_ERROR) {
                    ESP_LOGE(TAG, "C6 radio error %d", msg.u.status.last_radio_err);
                }
                run_actions(lora_sup_on_status(&sup, &msg.u.status, now_ms()));
                break;
            case LORA_MSG_RESULT:
                if (msg.u.result.for_type == LORA_MSG_SEND) {
                    portENTER_CRITICAL(&state_lock);
                    lora_tx_result(&tx, msg.u.result.code);
                    uint32_t took = now_ms() - tx_sent_ms, est = tx_airtime_ms;
                    portEXIT_CRITICAL(&state_lock);
                    // The bridge's round trip against MeshCore's airtime estimate (it gives up at 1.5 x estimate):
                    // only with "fw lora raw on" (the console shows info lines too, and it came with every send)
                    if (log_raw) {
                        ESP_LOGW(TAG, "tx answered after %lu ms (airtime estimate %lu ms)", (unsigned long)took,
                                 (unsigned long)est);
                    }
                }
                if (msg.u.result.code != LORA_RESULT_OK) {
                    ESP_LOGE(
                        TAG,
                        "C6 refused %s: code %u radio %d",
                        msg_name(msg.u.result.for_type),
                        msg.u.result.code,
                        msg.u.result.radio_err
                    );
                }
                break;
            default:
                portENTER_CRITICAL(&state_lock);
                p4_bad++;
                portEXIT_CRITICAL(&state_lock);
                break;
        }
    }
}

bool lora_start(void) {
    load_config();
    boot_config = config;
    rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(lora_rx_msg_t));
    if (!rx_queue) {
        ESP_LOGE(TAG, "no memory for the receive queue");
        return false;
    }
    if (eh_host_peer_data_register(LORA_MSG_ID_TO_P4, on_peer_data, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "peer data callback not registered");
        return false;
    }
    console_register("lora", "lora [raw on|off] (radio status; raw: print packets as hex and tx answer times)", cmd_lora);
    if (xTaskCreatePinnedToCore(lora_task, "LoRa", 6144, NULL, 3, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "task not started");
        return false;
    }
    __atomic_store_n(&started, true, __ATOMIC_RELEASE);
    return true;
}

bool lora_up(void) {
    return __atomic_load_n(&started, __ATOMIC_ACQUIRE) && !__atomic_load_n(&stopped, __ATOMIC_ACQUIRE);
}

void lora_stop(void) {
    if (__atomic_load_n(&stopped, __ATOMIC_ACQUIRE) || !rx_queue) {
        return;
    }
    __atomic_store_n(&stop_requested, true, __ATOMIC_RELEASE);
    // The task checks at the top of its loop, so within a second of the queue wait (plus a send to the C6)
    for (int i = 0; i < 400 && !__atomic_load_n(&stopped, __ATOMIC_ACQUIRE); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
}

void lora_get_stats(lora_ui_stats_t *out) {
    portENTER_CRITICAL(&state_lock);
    out->packets         = packets;
    out->last_rssi_dbx10 = last_rssi_x10;
    snprintf(out->last_summary, sizeof(out->last_summary), "%s", last_summary);
    portEXIT_CRITICAL(&state_lock);
}

static void cmd_lora(char const *args) {
    if (strcmp(args, "raw on") == 0 || strcmp(args, "raw off") == 0) {
        log_raw = args[5] == 'n';
        console_reply("lora raw %s", log_raw ? "on" : "off");
        return;
    }
    if (*args) {
        console_reply("lora [raw on|off]");
        return;
    }
    lora_status_t st;
    bool          have;
    uint32_t      age_s, dropped, bad;
    portENTER_CRITICAL(&state_lock);
    st      = c6_status;
    have    = have_status;
    age_s   = (now_ms() - status_at_ms) / 1000;
    dropped = p4_dropped;
    bad     = p4_bad;
    portEXIT_CRITICAL(&state_lock);

    char line[STATUS_LINE_MAX];
    lora_status_format(&config, &st, have, age_s, dropped, bad, line, sizeof(line));
    console_reply("lora %s", line);
}
