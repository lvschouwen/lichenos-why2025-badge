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
//   0x11 psram.*     PSRAM clock/tuning/heap figures (#316), served by the P4; row file reg_p4_psram.c
//   0x12 disp.*      display/DSI (#317); reg_p4_display.c
//   0x13 sens.*      sensors (#318); reg_p4_sensors.c
//   0x14 (misc)      haptics.*, led.*, tz, hold.* (GPIO holds) (#319); reg_p4_misc.c
//   0x15 settings.*  write status of the settings store (#320); reg_p4_settings.c
//   0x16-0x1F    reserved for further P4-side domains
// Each P4-side domain has its own row file under firmware/components/lichen_os/ and appends its ids here in its block.
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
    REG_ID_WIFI_TXPWR    = 0x0302, // wifi.txpwr     I16 0.25 dBm (esp_wifi_get_max_tx_power), RO + TX: fw wifi tx (#311)
    REG_ID_WIFI_BW       = 0x0303, // wifi.bw        U8 MHz (esp_wifi_get_bandwidth), RO + TX: fw wifi bw
    REG_ID_WIFI_PS       = 0x0304, // wifi.ps        ENUM none / min (wifi_ps_type_t), settable, not saved

    // 0x04 Bluetooth
    REG_ID_BT_STATE = 0x0400, // bt.state  ENUM esp_bt_controller_status_t (idle / inited / enabled)

    // 0x0F C6 meta
    REG_ID_C6_REG_CAPS = 0x0F00, // c6.reg_caps  U32, RO: REG_CAP_* bits of the domains this C6 serves

    // 0x10 P4 backlight state (#310): the P4 driver's mode, served by the P4, never on the wire
    REG_ID_BL_IDLE     = 0x1000, // bl.idle      ENUM, RO: bright / dim / off (the screen timeout)
    REG_ID_BL_OVERRIDE = 0x1001, // bl.override  ENUM, RO: none / thermal / flashlight / pulse

    // 0x11 psram (#316): the MSPI controller's registers as they are now, served by the P4, never on the wire; all RO
    // (`fw psram dl` stays the only mutator). Clock = source / core divider / bus divider.
    REG_ID_PSRAM_CLK      = 0x1100, // psram.clk       U32 Hz, the PSRAM bus clock the dividers give (80 or 120 MHz)
    REG_ID_PSRAM_CLK_SRC  = 0x1101, // psram.clk_src   ENUM xtal / mpll / spll / cpll (HP_SYS_CLKRST psram_clk_src_sel)
    REG_ID_PSRAM_SRC_CLK  = 0x1102, // psram.src_clk   U32 Hz, that source (its PLL registers, read at boot)
    REG_ID_PSRAM_CORE_DIV = 0x1103, // psram.core_div  U32, the core clock divider (div_num + 1)
    REG_ID_PSRAM_BUS_DIV  = 0x1104, // psram.bus_div   U32, the SPI_MEM_S_SRAM_CLK divider (1 = equal to the core clock)
    REG_ID_PSRAM_WANT     = 0x1105, // psram.want      U32 MHz, what the speed switch asks for (80 / 120, #121)
    REG_ID_PSRAM_DL_IDX   = 0x1106, // psram.dl_idx    U8, the delayline idx 0..30 the data / DQS delays below make
    REG_ID_PSRAM_DL_DATA  = 0x1107, // psram.dl_data   U8, the data pins' delay (pin D)
    REG_ID_PSRAM_DL_DQS   = 0x1108, // psram.dl_dqs    U8, the DQS0 delay (90 degree tap)
    REG_ID_PSRAM_PHASE    = 0x1109, // psram.phase     U8, the DQS0 phase id 0..3
    REG_ID_PSRAM_BOOT_IDX = 0x110A, // psram.boot_idx  U8, the delayline idx the boot tuning chose (#40)

    // 0x12 disp: the DSI/DPI link as the display driver runs it (#317), all RO, served by the P4
    REG_ID_DISP_PCLK      = 0x1200, // disp.pclk       U32 Hz, the DPI pixel clock read from the clock tree and divider
    REG_ID_DISP_REFRESH   = 0x1201, // disp.refresh    U32 0.1 Hz, pclk / (htotal * vtotal) of the panel timings
    REG_ID_DISP_LANES     = 0x1202, // disp.cfg_lanes     U8, DSI data lanes: configured, no IDF read-back
    REG_ID_DISP_LANE_RATE = 0x1203, // disp.cfg_lane_rate U32 Mbps per lane: configured, no IDF read-back
    REG_ID_DISP_PHY_MV    = 0x1204, // disp.cfg_phy_mv    U32 mV, DSI PHY LDO: configured, no IDF read-back
} reg_id_t;

#define REG_ID_DOMAIN(id) ((unsigned)(id) >> 8)

// c6.reg_caps: bit n set = the C6 serves domain 0x0n. The P4 asks it first: no REG_VAL at all means a C6 without the
// registry ("not supported by this C6"); a missing bit means this C6 predates that domain, so its keys are not asked.
#define REG_CAP(domain) (1u << (domain))
#define REG_CAP_BL      REG_CAP(0x01)
#define REG_CAP_LORA    REG_CAP(0x02)
#define REG_CAP_BT      REG_CAP(0x04)
#define REG_CAP_C6      REG_CAP(0x0F)
