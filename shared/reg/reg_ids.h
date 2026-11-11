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
    REG_ID_PSRAM_SRC_CLK  = 0x1102, // psram.src_clk   U32 Hz, that source: a boot snapshot of its PLL registers (PSRAM
                                    // init acquires the MPLL, so it cannot change later); psram.clk divides it
    REG_ID_PSRAM_CORE_DIV = 0x1103, // psram.core_div  U32, the core clock divider (div_num + 1)
    REG_ID_PSRAM_BUS_DIV  = 0x1104, // psram.bus_div   U32, the SPI_MEM_S_SRAM_CLK divider (1 = equal to the core clock)
    REG_ID_PSRAM_WANT     = 0x1105, // psram.want      U32 MHz, what the speed switch asks for (80 / 120, #121)
    REG_ID_PSRAM_DL_IDX   = 0x1106, // psram.dl_idx    U8, the delayline idx 0..30 the data / DQS delays below make
    REG_ID_PSRAM_DL_DATA  = 0x1107, // psram.dl_data   U8, the data pins' delay (pin D)
    REG_ID_PSRAM_DL_DQS   = 0x1108, // psram.dl_dqs    U8, the DQS0 delay (90 degree tap)
    REG_ID_PSRAM_PHASE    = 0x1109, // psram.phase     U8, the DQS0 phase id 0..3
    REG_ID_PSRAM_BOOT_IDX = 0x110A, // psram.boot_idx  U8, the delayline idx the boot tuning chose (#40)
    // The speed switch's last failure (#329), from its NVS record (psram_speed_logic.h), not from a register
    REG_ID_PSRAM_FAIL_WHY   = 0x110B, // psram.fail_why    ENUM none / boot_check / dma_gate / crash / restart / aborted /
                                      // gave_up / crash_armed / against_rec / power_resets
    REG_ID_PSRAM_FAIL_RST   = 0x110C, // psram.fail_rst    STR, the reset reason of the boot that judged it ("panic", "none")
    REG_ID_PSRAM_FAIL_AT120 = 0x110D, // psram.fail_at120  BOOL, the failed boot did run at 120 MHz (REACHED marker found or this boot)
    REG_ID_PSRAM_FAIL_FW    = 0x110E, // psram.fail_fw     STR, the image that judged it (fw-<sha>, cut at 15; "none")
    REG_ID_PSRAM_ABORTS     = 0x110F, // psram.aborts      U8, armed trials in a row that never reached 120 MHz (3 = given up)

    // 0x12 disp: the DSI/DPI link as the display driver runs it (#317), all RO, served by the P4
    REG_ID_DISP_PCLK      = 0x1200, // disp.pclk       U32 Hz, the DPI pixel clock read from the clock tree and divider
    REG_ID_DISP_REFRESH   = 0x1201, // disp.refresh    U32 0.1 Hz, pclk / (htotal * vtotal) of the panel timings
    REG_ID_DISP_LANES     = 0x1202, // disp.cfg_lanes     U8, DSI data lanes: configured, no IDF read-back
    REG_ID_DISP_LANE_RATE = 0x1203, // disp.cfg_lane_rate U32 Mbps per lane: configured, no IDF read-back
    REG_ID_DISP_PHY_MV    = 0x1204, // disp.cfg_phy_mv    U32 mV, DSI PHY LDO: configured, no IDF read-back

    // 0x14 haptics, status LED, time zone, C6 pad holds (#319): all read from the hardware or libc, RO
    REG_ID_HAPTICS_DUTY = 0x1400, // haptics.duty  U8 %, RO: the LEDC duty the motor pin is driven with
    REG_ID_LED_PAD      = 0x1401, // led.pad       STR, RO: the output pad level and what it means ("high/on")
    REG_ID_TZ           = 0x1402, // tz.zone       STR, RO: the TZ environment value as set (tzset gives no accept/reject), zone part only: "CET-1CEST"
    REG_ID_HOLD_GPIO12  = 0x1403, // hold.gpio12   BOOL, RO: pad hold latched on the C6 EN line
    REG_ID_HOLD_GPIO13  = 0x1404, // hold.gpio13   BOOL, RO: pad hold latched on the C6 boot strap

    // 0x15 settings: the settings service's write failures (#320), RO
    REG_ID_SETTINGS_WRITE_FAIL = 0x1500, // settings.write_fail  U32, RO: failed NVS writes since boot, saturating
    REG_ID_SETTINGS_LAST_ERR   = 0x1501, // settings.last_err    STR, RO: last failure "<key> 0x<esp_err>", key cut to fit 15 chars, no namespace

    // 0x13 sens: what the BMI270 and BME690 are configured to, read back from the chips (#318). All RO; the value of an
    // ENUM is the chip's own register code (names in reg_p4_sensors.c); the low byte is sensors_cfg_key_t
    REG_ID_SENS_ACC_ODR    = 0x1300, // sens.acc_odr     ENUM, RO: accel output data rate
    REG_ID_SENS_ACC_RANGE  = 0x1301, // sens.acc_range   ENUM, RO: accel full scale
    REG_ID_SENS_ACC_BWP    = 0x1302, // sens.acc_bwp     ENUM, RO: accel filter bandwidth / averaging
    REG_ID_SENS_ACC_PERF   = 0x1303, // sens.acc_perf    ENUM, RO: accel filter power / performance mode
    REG_ID_SENS_GYR_ODR    = 0x1304, // sens.gyr_odr     ENUM, RO: gyro output data rate
    REG_ID_SENS_GYR_RANGE  = 0x1305, // sens.gyr_range   ENUM, RO: gyro full scale
    REG_ID_SENS_GYR_BWP    = 0x1306, // sens.gyr_bwp     ENUM, RO: gyro filter bandwidth
    REG_ID_SENS_GYR_PERF   = 0x1307, // sens.gyr_perf    ENUM, RO: gyro filter power / performance mode
    REG_ID_SENS_ENV_OS_T   = 0x1308, // sens.env_os_t    ENUM, RO: BME690 temperature oversampling
    REG_ID_SENS_ENV_OS_P   = 0x1309, // sens.env_os_p    ENUM, RO: pressure oversampling
    REG_ID_SENS_ENV_OS_H   = 0x130A, // sens.env_os_h    ENUM, RO: humidity oversampling
    REG_ID_SENS_ENV_FILTER = 0x130B, // sens.env_filter  ENUM, RO: IIR filter coefficient
    REG_ID_SENS_HEAT_ON    = 0x130C, // sens.heat_on     BOOL, RO: gas heater enabled
    REG_ID_SENS_HEAT_TEMP  = 0x130D, // sens.heat_temp   U32 C, RO: heater target, from the chip's resistance register
    REG_ID_SENS_HEAT_MS    = 0x130E, // sens.heat_ms     U32 ms, RO: heater duration
} reg_id_t;

#define REG_ID_DOMAIN(id) ((unsigned)(id) >> 8)

// c6.reg_caps: bit n set = the C6 serves domain 0x0n. The P4 asks it first: no REG_VAL at all means a C6 without the
// registry ("not supported by this C6"); a missing bit means this C6 predates that domain, so its keys are not asked.
#define REG_CAP(domain) (1u << (domain))
#define REG_CAP_BL      REG_CAP(0x01)
#define REG_CAP_LORA    REG_CAP(0x02)
#define REG_CAP_BT      REG_CAP(0x04)
#define REG_CAP_C6      REG_CAP(0x0F)
