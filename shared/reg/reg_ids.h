// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
#pragma once

// The numeric ids of the settings registry (#307/#308). They are wire format between the P4 and the C6 (REG_GET /
// REG_SET / REG_VAL in shared/lora/lora_proto.h) and the stable name of a key in logs and tools:
// - append-only: an id is never renumbered or reused, a key that goes away leaves its number as a gap;
// - the high byte is the domain, the low byte counts up from 0x00 within it, so a domain grows without moving others;
// - the dotted names ("lora.freq") live only in the tables that serve or mirror a key, never on the wire.
// Which chip serves a key is in its table, not in the id: the P4 asks the C6 for C6 keys and serves its own.
//
// Domains (high byte):
//   0x01 bl.*    backlight, served by the C6 (it drives the PWM)
//   0x02 lora.*  the SX1262, served by the C6; radio keys are read-only (TX) in the registry, see #307
//   0x03 wifi.*  Wi-Fi, served by the P4 through the esp_hosted RPCs
//   0x04 bt.*    Bluetooth controller, served by the C6
//   0x05-0x0E    reserved for further C6-side domains
//   0x0F c6.*    C6 meta
//   0x10 bl.*    the P4's backlight driver state (bl.idle, bl.override; #310)
//   0x11-0x1F    reserved for P4-side domains (wave 2, #312: PSRAM, display/DSI, sensors, haptics, LED, GPIO holds)
typedef enum {
    // 0x01 backlight: the brightness the C6 drives now, in percent (after its 10-80 % clamp; 0 = off)
    REG_ID_BL_DISPLAY  = 0x0100, // bl.display   U8 %
    REG_ID_BL_KEYBOARD = 0x0101, // bl.keyboard  U8 %
    REG_ID_BL_FREQ     = 0x0102, // bl.freq      U32 Hz, RO: the PWM frequency the LEDC timer runs (#309)
    REG_ID_BL_BOOT     = 0x0103, // bl.boot      BOOL, RO: on = still the C6's own boot level, no command since its start

    // 0x02 LoRa: what the SX1262 runs now (#306: the C6's driver state, not the P4's request)
    REG_ID_LORA_FREQ     = 0x0200, // lora.freq      U32 Hz
    REG_ID_LORA_BW       = 0x0201, // lora.bw        U32 Hz
    REG_ID_LORA_SF       = 0x0202, // lora.sf        U8
    REG_ID_LORA_CR       = 0x0203, // lora.cr        U8, coding rate denominator 5..8 = 4/5..4/8
    REG_ID_LORA_SYNC     = 0x0204, // lora.sync      U8 sync word
    REG_ID_LORA_PREAMBLE = 0x0205, // lora.preamble  U32 symbols
    REG_ID_LORA_POWER    = 0x0206, // lora.power     I16 dBm
    REG_ID_LORA_STATE    = 0x0207, // lora.state     ENUM lora_state_t

    // 0x03 Wi-Fi
    REG_ID_WIFI_COUNTRY  = 0x0300, // wifi.country   STR, ISO 3166 alpha-2 (P4-local, never on the wire)
    REG_ID_WIFI_PROTOCOL = 0x0301, // wifi.protocol  U8 bitmask of WIFI_PROTOCOL_11B/G/N/AX

    // 0x04 Bluetooth
    REG_ID_BT_STATE = 0x0400, // bt.state  ENUM esp_bt_controller_status_t (idle / inited / enabled)

    // 0x0F C6 meta
    REG_ID_C6_REG_CAPS = 0x0F00, // c6.reg_caps  U32, RO: REG_CAP_* bits of the domains this C6 serves

    // 0x10 P4 backlight state (#310): the P4 driver's mode, served by the P4, never on the wire
    REG_ID_BL_IDLE     = 0x1000, // bl.idle      ENUM, RO: bright / dim / off (the screen timeout)
    REG_ID_BL_OVERRIDE = 0x1001, // bl.override  ENUM, RO: none / thermal / flashlight / pulse
} reg_id_t;

#define REG_ID_DOMAIN(id) ((unsigned)(id) >> 8)

// c6.reg_caps: bit n set = the C6 serves domain 0x0n. The P4 asks it first: no REG_VAL at all means a C6 without the
// registry ("not supported by this C6"); a missing bit means this C6 predates that domain, so its keys are not asked.
#define REG_CAP(domain) (1u << (domain))
#define REG_CAP_BL      REG_CAP(0x01)
#define REG_CAP_LORA    REG_CAP(0x02)
#define REG_CAP_BT      REG_CAP(0x04)
#define REG_CAP_C6      REG_CAP(0x0F)
