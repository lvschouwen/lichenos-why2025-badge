#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Messages between the P4 and the C6's LoRa service, carried over ESP-Hosted peer data (#51).
// Pure logic, no ESP headers: built into the C6 image, the P4 firmware and the host tests.
//
// Every message starts with [version][type]; the fields follow little-endian in the order of the structs below.

#define LORA_PROTO_VERSION 3 // 2: STATUS carries the C6 uptime, heap and stack (#56); 3: transmit (#65)
#define LORA_MSG_ID_TO_C6  0x4C01
#define LORA_MSG_ID_TO_P4  0x4C02
#define LORA_MSG_MAX       300 // largest encoded message (RX_PACKET with 255 bytes is 266)

typedef enum {
    // P4 -> C6
    LORA_MSG_CONFIG = 1,
    LORA_MSG_RX_START,
    LORA_MSG_RX_STOP,
    LORA_MSG_STATUS_REQ,
    LORA_MSG_SEND, // a packet to transmit; answered by a RESULT once it left the radio (or failed)
    // C6 -> P4
    LORA_MSG_RX_PACKET = 0x81,
    LORA_MSG_STATUS,
    LORA_MSG_RESULT,
} lora_msg_type_t;

typedef struct {
    uint32_t freq_hz;
    uint32_t bw_hz;
    uint8_t  sf;
    uint8_t  cr; // coding rate denominator: 5 = 4/5 ... 8 = 4/8
    uint8_t  sync_word;
    uint16_t preamble;
    int8_t   tx_power_dbm; // -9..22 (SX1262 high-power PA)
} lora_config_t;

// The local MeshCore mesh: 869.618 MHz, 62.5 kHz, SF7, CR 4/5, sync word 0x12, preamble 16, 22 dBm as MeshCore's
// badge variant
#define LORA_CONFIG_DEFAULT {869618000, 62500, 7, 5, 0x12, 16, 22}

typedef enum {
    LORA_STATE_UNCONFIGURED,
    LORA_STATE_IDLE,
    LORA_STATE_RX,
    LORA_STATE_ERROR,
    LORA_STATE_TX,
} lora_state_t;

typedef enum {
    LORA_RESULT_OK,
    LORA_RESULT_REFUSED,
    LORA_RESULT_BAD_PARAM,
    LORA_RESULT_RADIO_ERROR,
    LORA_RESULT_BAD_STATE,
} lora_result_code_t;

typedef struct {
    int16_t  rssi_x10; // dBm * 10
    int16_t  snr_x10;  // dB * 10
    uint32_t c6_ms;
    uint8_t  len; // 1-255
    uint8_t  data[255];
} lora_rx_packet_t;

typedef struct {
    uint8_t len; // 1-255
    uint8_t data[255];
} lora_tx_packet_t;

typedef struct {
    uint8_t  state; // lora_state_t
    int16_t  rssi_x10;
    int16_t  last_radio_err; // RadioLib status code, 0 = none
    uint32_t packets;
    uint32_t crc_errors;
    uint32_t header_errors;
    uint32_t dropped;
    uint32_t ignored;    // messages from the P4 that did not decode
    uint32_t uptime_s;   // since the C6 booted: a drop means it restarted
    uint32_t heap_free;  // bytes
    uint32_t heap_min;   // lowest free heap since boot, bytes
    uint16_t stack_free; // LoRa task stack high-water mark, bytes
} lora_status_t;

typedef struct {
    uint8_t for_type; // lora_msg_type_t of the command answered
    uint8_t code;     // lora_result_code_t
    int16_t radio_err;
} lora_result_t;

typedef struct {
    uint8_t type; // lora_msg_type_t
    union {
        lora_config_t    config;
        lora_rx_packet_t rx;
        lora_tx_packet_t tx;
        lora_status_t    status;
        lora_result_t    result;
    } u;
} lora_msg_t;

// Bytes written, or 0 if cap is too small or the type is unknown
size_t lora_encode(lora_msg_t const *msg, uint8_t *buf, size_t cap);

typedef enum {
    LORA_DEC_OK,
    LORA_DEC_BAD_VERSION,
    LORA_DEC_BAD_TYPE,
    LORA_DEC_BAD_LEN,
} lora_dec_t;

// The length must match the type exactly
lora_dec_t lora_decode(uint8_t const *buf, size_t len, lora_msg_t *out);

// Within what the RA-01SH (862-928 MHz) and the SX1262 support
bool lora_config_valid(lora_config_t const *c);

#ifdef __cplusplus
}
#endif
