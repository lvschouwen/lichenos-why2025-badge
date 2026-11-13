# Epic #394 plan: hardware and power

Read-only, main @ 47dc849e, 2026-11-13. Code status checked in the tree, not taken from titles.

## 1. Issue inventory (13 open; no issue carries `now`)

| # | Short title | Labels of note | Real status (code) |
|---|---|---|---|
| 292 | Headless `fw poweroff` restarts | bug, score 7, needs-lucas | Done in code (`power_route.c`, `power_off_headless`, main.c:248-290, power_route_test). Awaits bench (#281 steps). |
| 39 | Lit restart, early boot screen, quiet bootloaders | score 6, needs-lucas | Mostly done (lit "Restarting...", screen before storage, C6 bootloader WARN). Open: cold-boot by-eye check (steps 2-3), lit restart via #134, P4 bootloader WARN (USB only, sdkconfig.defaults not yet changed). |
| 146 | Wi-Fi RTT degradation | bug, score 6 | Fixes on main (PS off, RLE screenshot, own send task, diagnostics). Open: badge-vs-plug soak. |
| 164 | Message light pulse + flashlight | score 6, needs-lucas | Done in code (31b04c79). Awaits by-eye bench (3 steps posted). |
| 12 | Hold backlight via R16 | score 5, needs-lucas | Code on main (`r16_hold.c`, `fw bl r16`), R16 not fitted. Parked needs-hardware (Lucas 2026-11-07). |
| 73 | BLE toggle + MeshCore companion | score 5 | T1 done (`RADIO_MASK_DEFAULT` excludes BT). S0 probe measured (throwaway branch). No `lichen_ble`, no NimBLE config on P4. Not started otherwise. |
| 372 | Vibration check after GPIO8 pull-down | score 5, needs-lucas | Bench only, waits on the R49 fix. |
| 388 | Power profiles + light sleep | score 5 | Not started. No `CONFIG_PM_ENABLE`, only deep sleep in `lichen_sys_power.c`. Light sleep back in scope (Lucas 2026-11-13). |
| 11 | Battery bodges (MAX17048 chosen) | score 4 | Partly: pure `battery_model.c` + test (1C). No I2C2 bus (board_i2c is I2C0 only), no driver, service or UI. |
| 67 | MAX98357A audio | score 4 | Partly: pure `audio_pattern.c` + test (1D). No I2S driver. |
| 68 | Vibration motor R49 | score 4 | Software done (`lichen_haptics`). Only probe J3 pin 6 + R49 bridge remain. |
| 172 | 802.15.4 scanner | score 4 | Partly: `shared/i154` protocol + test (1E). No C6 service, P4 service or app. |
| 370 | RadioLib 8.0.0 | needs-proofing | Merged 2001645a, on-air verified. Open: CRC-error rate over >= 1 h. |

## 2. Dependencies inside the epic

- #68 bodge (R49) before #372 (same check) and before any haptics feel test.
- #11 driver (W2) before #11 UI (W5), and before #388 profiles can show or use battery state (optional input, not a hard block).
- #388 measurement (W0) before any #388 code beyond the pure table: the profile numbers come from the USB meter.
- #146 soak and the #388 Wi-Fi-mode column: #388 re-enables a Wi-Fi PS policy; #146 must be closed (or its finding known) first so PS-min is not re-introduced blind.
- #67 driver and #68/#372: one merged feedback registration (`haptics.c`, `notify_os.h`); audio joins it, so haptics must stay working (host test both).
- #73 and #172 both add C6 coexistence load on one antenna; do not bench them in the same wave.
- #39 restart-path work and #12 both touch `board/backlight.c`; #12 is parked, so no conflict now.

## 3. Cross-epic dependencies

Needs from others:
- **#216 / #234** (P4 <-> C6 contract + two-chip gate): before W6 (#172 C6 service, new ids 0x4C03/04 + CAPS), W7 (#73 C6 BT off), W8 (#388 wake via C6/LoRa), and the C6 side of #39 already shipped. Every C6 change: `ota-c6` first.
- **#223** (c6link drops upward includes): before #172 3A P4 service and #73 C6 BT-off hook.
- **#226** (lichen_sys_display / lichen_sys_power): before #11 UI (W5), #67 settings (W4b), #388 (W8). Also wraps the soft-restart path #39 touched.
- **#229** (CAP_*): CAP_BATTERY (#11 W5), CAP_AUDIO (#67 W4b), CAP_RADIO_SCAN (#172 app), CAP_BT (#73 T7).
- **#235** (one NVS owner): audio volume/key-click and haptics settings through the settings service, not `nvs_open`.
- **#224** (backlight NVS to settings service): #388 dim/off times and flashlight restore touch the same keys.
- **#262** (internal RAM/DMA budget): #67 I2S DMA buffers and #73 NimBLE (~7 KB internal) need a measured budget; #234's DMA free >= 150 KB gate.
- **#393** UI: #386 quick-settings panel reuses #164's flashlight; #11 status bar icon and Settings > Battery page follow the UI standard.
- **#381** remote bench: a remote current/battery read-back (`fw battery`, `fw pm`) makes later checks remote; dev-mode gate for new debug commands (`fw beep`, `fw pm sleep test`).
- **#395** security: #73 pairing (SC-only, passkey, command filter) needs a security review.

Others need from this epic:
- **#392** MeshCore: #162 telemetry battery value needs #11 W2; #73 changes MyMesh pipe ownership (handover).
- **#393**: Settings > Battery, Sound, Bluetooth pages; #386 needs #164 closed.
- **#262**: #388 light sleep interacts with PSRAM 120 MHz (#353, closed) and DMA.

## 4. Decisions for Lucas, missing prerequisites

Decisions:
1. #388: confirm the profile design after W0 numbers (Camp / Normal / Always-on fields; whether light sleep is allowed with Wi-Fi associated or only with Wi-Fi off). Also: main only, or Opus agents for the pure parts (rec: pure table by agent, sleep enable in main).
2. #67: is a speaker fitted on LS1 (connector ships without one)? Gates the audible bench, not the code.
3. #73: phone for spike S1/S2; is a passkey+bond acceptable if the MeshCore app needs a fallback?
4. #39: close after steps 2-4 and split the P4-bootloader WARN into its own USB-session issue (rec), or keep it open.
5. #12, #68, #372: when to solder (R16 bridge, R49 bridge). #12 stays parked.
6. #370: accept the CRC rate if the 1 h number is within noise of the old 27 %.

Missing prerequisites:
- MAX17048 breakout wired on J7 (3V3/GND/SDA GPIO22/SCL GPIO9, CELL+ to +BATT). Without it W2 can only prove the "absent" path.
- Inline USB meter (#388 W0); for light-sleep current a meter with mA resolution. Battery-only measurement only under Lucas's supervision, never an unattended drain test.
- Scope or logic probe on J3 pin 6 for the 20 kHz PWM (#68).
- Phone with nRF Connect + MeshCore app (#73 S1/S2).
- MacBook USB session for the P4 bootloader (#39).

## 5. Waves

**E394-W0 (main only, no code).** Issues #292, #146, #370, #39, #164, #68/#372, #388-measure.
- Remote: #146 soak (badge vs reference device, same node, 5 min + 30 min). #370 CRC count over >= 1 h (`fw lora`). #292 part 1: `fw noscreen`, reboot, `fw poweroff`; console must not return after 75 s (a restart would).
- Lucas's hands: #292 part 2 (power button wakes it, cold boot clean). #39 steps 2-3 (cold boot, no garbage frame, time to logo). #164 three by-eye steps. #68 probe J3 pin 6, then R49 bridge, then #372. #388 baseline current with the USB meter: screen on / dimmed / off, Wi-Fi PS none vs min (`fw wifi ps`), LoRa RX idle; badge on USB, readings by hand.
- Acceptance: each issue closed with evidence or commented with the numbers. Output feeds decision 1.

**E394-W1 (#11 driver + service, part 1).** Prereq none (can run alongside #216 L-0). Model **Opus** (new bus on the camera-header pins, and the service can latch an automatic power-off).
- Tasks: I2C2 bus in `board_i2c` (GPIO22/GPIO9, own port, absent-tolerant probe of 0x36 and 0x48); MAX17048 driver (VCELL, SOC, CRATE, VERSION) in `battery/`; battery service (poll, `battery_model` decisions, low-warn event, 3.30 V latched off **disabled by default until calibrated**); `fw battery` read-back.
- Host tests: a pure `battery_service_step()` (absent/present, read errors, dwell, latch, no power-off when absent or when the gauge reports nonsense); fails before, passes after.
- Bench: remote without breakout (absent path, no boot delay, `fw i2c scan` unchanged on I2C0). With breakout (Lucas wires it): remote read-back of V/%, compared once against Lucas's meter.
- Files: `board/board_i2c.[ch]`, `battery/*`, a new register entry, main boot step. Conflict files: `battery/CMakeLists.txt`, `board/CMakeLists.txt`, `host_tests/CMakeLists.txt`, `tools/standard-baseline.txt`, `tools/layers.conf` (if a service component is added), `boot_order[]`.
- Reviewer: I2C error handling and timeouts, never powering off on a bad read, no new task stack in PSRAM.

**E394-W2 (#67 I2S driver).** Prereq: #262 DMA budget measured (#234 gate value known). Parallel with W1 (disjoint files except conflict files). Model **Opus** (DMA/internal RAM, feedback registration).
- Tasks: I2S std mode, BCLK 26, LRCK 25, DIN 27, GPIO24 as plain SD_MODE (high = on, no MCLK), small internal DMA buffers via the memory book, amp off between sounds; `fw beep` behind dev mode (#381); audio joins the one feedback registration in `lichen_os` next to haptics.
- Host tests: amp-gating / queue decision (pure), feedback dispatch to both audio and haptics (extend haptics or notify tests).
- Bench: remote: `fw mem` DMA free >= 150 KB after init, no I2S errors in log, SD_MODE low when idle (readback). Audible check needs Lucas and a speaker (decision 2).
- Files: `lichen_audio/*`, `lichen_os` feedback registration, `lichen_haptics/haptics.c` (registration only). Conflict: `lichen_audio/CMakeLists.txt`, `host_tests/CMakeLists.txt`, `tools/standard-baseline.txt`, `tools/gate_manifest.txt` if a gate is added, `boot_order[]`.
- Reviewer: DMA placement, ISR/IRAM use, rate limit, haptics regression.

**E394-W3 (#172 C6 802.15.4 service).** Prereq: #234 contract section for 0x4C03/04 + CAPS. Model **Opus** (changes what the badge transmits; P4 <-> C6 contract).
- Tasks: `c6/main/i154_service.c`: enable only during a scan, ED sweep, promiscuous RX into the shared dedupe, active beacon request only with an explicit flag, `disable()` after; peer-data handler.
- Host tests: already-merged codec; add a pure scan-session state machine (start, timeout cap 60 s, stop always disables, active refused without the flag).
- Bench: remote, `ota-c6` first: IDENT match, ED + passive on one channel, iperf with/without scan (15 min, go/no-go). Ask Lucas before the first active scan on air (bench rules).
- Files: `c6/main/*`, `c6/CMakeLists.txt`/`c6/main/CMakeLists.txt`, `c6/sdkconfig.defaults` (driver linked). Conflict: component CMakeLists, `sdkconfig.defaults` (c6), `tools/standard-baseline.txt`, `host_tests/CMakeLists.txt`.
- Security: bounded parser already fuzzed; check the C6 RX path never forwards unbounded frames.

**E394-W4 (#172 P4 service + #67 settings).** Prereq: W3 + #223 (#172); W2 + #226, #229 CAP_AUDIO, #235 (#67). Two parallel tasks:
- 4a #172 P4 `c6link` handler + `lichen_i154` service with the "update the C6" path. Opus (contract). Host test: CAPS absent -> "not supported", no hang.
- 4b #67 Sound settings (volume, key click) via the settings service + sys surface. Sonnet. Host test: setting -> pattern level mapping. Bench: remote screenshots + `fw ui overflow` 0; audible part Lucas.
- Then (after #229 CAP_RADIO_SCAN) 4c #172 Scanner app, Sonnet, UI standard; Active scan behind the careful confirm.

**E394-W5 (#11 UI).** Prereq W1, #226, #229 CAP_BATTERY. Sonnet.
- Status-bar icon with % and charging trend, Settings > Battery & power, 3.40 V warning screen; enable the 3.30 V auto-off only after a calibration note from Lucas.
- Host tests: icon level / text formatting, warning hysteresis (pure). Bench: remote screenshots, `fw ui overflow`; real low-voltage behaviour is **not** tested by draining the cell; inject values through a dev-mode `fw battery fake` instead.
- Conflict: shell part lists, `host_tests/CMakeLists.txt`, `tools/standard-baseline.txt`.

**E394-W6 (#73 BLE, part 1).** Prereq #234 (C6 BT off), #223, #262 budget. Parallel tasks, disjoint:
- T3/T4 pure `companion_filter.c` + PSRAM frame ring + host tests (Sonnet).
- T2/T5 `lichen_ble` (NimBLE host-only, PSRAM pools, host task stack internal), Nordic UART GATT, SC-only pairing, bonds (Opus).
- T6 C6 `BT off` line + `esp_bt_controller_mem_release` (Opus, contract, `ota-c6` first).
- Bench: remote `fw mem` before/after, C6 heap after release. **Spike S1 needs Lucas + phone** (advertise, nRF Connect, an SD job, a forced C6 reset) and gates W6b.
- Conflict: `firmware/sdkconfig.defaults`, `tools/layers.conf` (lichen_ble from day one), component CMakeLists, `host_tests/CMakeLists.txt`, `tools/standard-baseline.txt`, `boot_order[]`. Security review (#395) of pairing and the command filter.

**E394-W6b (#73 part 2).** Prereq S1 pass. T7 `ble_serial.cpp` + handover (Opus, extend companion_test.cpp), T8 UI (Sonnet), T9 end-to-end with phone (Lucas), T10 docs (Sonnet).

**E394-W7 (#388 profiles, pure).** Prereq W0 numbers + decision 1. Sonnet.
- Pure profile -> policy table (dim/off times, Wi-Fi PS mode, LoRa duty, sleep allowed) and the inhibit rule (update, setup, bench run, screen on). Host tests as in the acceptance. No sleep enabled yet; profiles change dim/off and Wi-Fi PS only.
- Bench: remote read-back of the applied policy; current per profile by Lucas with the meter.

**E394-W8 (#388 light sleep).** Prereq W7, #226, #234 (wake path via C6/LoRa DIO1). **Opus, main session recommended, no parallel agents.**
- `CONFIG_PM_ENABLE` + automatic light sleep only when inhibit allows; wake on TCA8418 IRQ and C6/LoRa line; DSI pipeline paused, SDIO link kept valid.
- Host tests: inhibit + wake-source selection. Bench: key wake (Lucas), #badgetest packet wake with RX latency unchanged (remote), 1 h no unplanned reset (remote), current (Lucas, meter). Performance: LVGL frame time and PSRAM DMA gate after wake.
- Conflict: `firmware/sdkconfig.defaults`, `tools/standard-baseline.txt`, `host_tests/CMakeLists.txt`.

Concurrency: W1, W2, W3 can run in one agent batch (three agents, disjoint code, union-merge the conflict files, one at a time). W4/W5/W6 after their #216 prerequisites. W8 alone.

## 6. Risks and what to leave out

Risks:
- Light sleep (#388) against the DSI pipeline, SDIO esp_hosted link, PSRAM 120 MHz and missed LoRa packets: the highest-risk item; keep it last and in main.
- BLE HCI over SDIO with the SD card on SDMMC is unproven; C6 recovery with NimBLE alive is untested.
- A wrong battery reading must never power the badge off; keep the auto-off disabled until calibrated.
- New C6 code (W3, W6 T6) can lock out Wi-Fi recovery if it breaks the link: `ota-c6` first, USB at hand.
- The P4 bootloader flash (#39) is USB-only; never `flash-fw` without `--no-storage`.
- #146 root cause (stuck link after long upload) is unproven; #388's Wi-Fi PS policy may bring the symptom back.

Leave out of this program:
- #12: parked by Lucas until R16 is bridged; code exists.
- #68 / #372 software: none needed; only solder and feel checks (W0).
- #73 W6b and #172 4c until their spikes pass; they are large (ease 1) and depend on the phone and the iperf go/no-go.
- #11 mods 2-5 (ADC divider, KEY NMOS, USB sense, TMP102): optional follow-ups by Lucas's decision; file separately when wanted.
- #39's "skip the ROM-mode check via an app MD5 query": not in scope, would be its own issue (touches the #234 contract).
