# Lichen architecture: what runs where

Agreed with Lucas on 2026-10-30. Companion to [shell-design.md](shell-design.md) (the screens). Lichen is the OS on
the WHY2025 badge (lean firmware, #61); MeshCore is its first app (#67).

## Two chips

**P4 (Lichen):** dual-core RISC-V at 360 MHz (rev v1.0 silicon), 32 MB PSRAM at 80 MHz by default, 120 MHz through the runtime switch in Settings (a patched IDF option, #94, #121;
see docs/psram-speed-switch.md; at 200 MHz this board flips bits in PSRAM words, #89/#93, 80 MHz is the known-clean fallback; `fw psram` watches it
with canaries), 16 MB flash.
- Runs Lichen, the apps (MeshCore included, #33 option 1) and all UI.
- Owns the display (ST7703, MIPI DSI, 720×720, shown rotated 90°), the keyboard (TCA8418, I2C), the sensors (BMI270,
  BME690, same I2C bus), the SD card and the flash (two 4 MiB app slots with rollback, FAT `/storage`).

**C6 (radio coprocessor):** single-core RISC-V at 160 MHz.
- Runs ESP-Hosted (Wi-Fi, the BLE controller over VHCI) and our LoRa service (SX1262 on RA-01SH: receive, transmit,
  interrupts).
- Drives the screen and keyboard backlights (PWM).
- No application logic: whatever has to react to the radio within milliseconds stays here, everything else goes to
  the P4.

```
                    P4 (Lichen)                                C6 (radio coprocessor)
 display ST7703 ─MIPI DSI─┤                          SDIO 4-bit ├─ ESP-Hosted: Wi-Fi, BLE HCI, RPC,
 keyboard TCA8418 ─I2C────┤ ◄══════════════════════════════════►│   peer data = LoRa protocol v4
 BMI270 / BME690 ─I2C─────┤   UART: flashing the C6 at boot,    ├─ SPI ─ SX1262 (RA-01SH) + bodges
 SD card ─────────────────┤         backlight commands          ├─ PWM: screen + keyboard backlight
 flash: Lichen ×2, /storage│   GPIO12 → C6 EN (resets TCA8418 too)
                          │   GPIO13 → C6 boot strap, GPIO14 ← C6 INT
```

Links between the chips:
- **SDIO (4-bit):** ESP-Hosted carries Wi-Fi, BLE HCI, RPC and peer data. Our LoRa protocol v4 runs on peer data
  (IDs 0x4C01 P4→C6, 0x4C02 C6→P4, `shared/lora/lora_proto.h`). Both chips must run the same protocol version.
- **UART:** the P4 flashes the C6 at boot from `/storage/c6` when the image's MD5 differs; afterwards the same UART
  carries the backlight commands.
- **GPIO:** P4 GPIO12 → C6 EN, which also resets the TCA8418. GPIO13 → C6 boot strap; esp_hosted's `RESET_GPIO=13` is
  wrong, #15. GPIO14 ← C6 interrupt.
- **GPIO0/GPIO1:** XTAL_32K, the Y1 crystal for the RTC slow clock (#63). Reserved: nothing else may claim them.

## Two-chip contract

What the P4 (Lichen) and the C6 (radio coprocessor) rely on from each other (#234, part of #216). It must hold through
the whole v2 migration: a wave that touches any line below re-runs the two-chip bench gate (lichen-bench skill) before
it counts as merged. Line numbers are in `firmware/components/` unless noted and drift; the symbol names are the anchor.
Pure parts are pinned by `host_tests/two_chip_contract_test.c` (ids, CONFIG bytes, IDENT compare) on
top of `lora_proto_test`, `i154_proto_test`, `c6_boot_policy_test`, `c6_watch_test`, `c6_paths_test` and `r16_hold_test`.
No data migration: stored keys named here start from their defaults if they change.

**Layer rule:** `c6link` and `lora` are the L2 drivers that own the link, and `esp_hosted` (a vendored IDF component with
no layer line) is reached only through them; services reach the C6 only through them too (`tools/layers.conf` enforces
it: the `esp_hosted_*` and `eh_*` symbols are owner-only for `c6link` and `lora`). The C6 image (`c6/`, Apache-2.0) stays outside the P4 layer model and
is not refactored by v2. A C6-side change in a v2 wave needs `ota-c6` before the P4 image.

### 1. ESP-Hosted Wi-Fi over SDIO
- **No auto-init:** `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n` (`firmware/sdkconfig.defaults`). The C6
  flasher must release the C6 first; auto-init races it into a boot loop. Init order in `c6link_start`
  (`c6link/c6link.c:356`): flasher check (or skip), `hold_c6_boot(1)` (:377), `esp_hosted_init` under the SDMMC lock
  (:380, the SD card shares the controller, #60), `esp_hosted_connect_to_slave`, IDENT, then the net hook `attached`,
  which is `start_wifi` in `lichen_net/wifi_service.c` (#223). A runtime re-attach repeats deinit, init, connect in
  `c6link/c6_sup.c:290`; around it c6sup takes the Wi-Fi driver down and up through the net hooks `driver_down` /
  `driver_up` (the c6wifi task's MSG_C6_DOWN / MSG_C6_UP).
- **Hooks, not upward calls (#223):** c6link (L2) reaches the boot step and the Wi-Fi service only through
  `c6link/c6_hooks.h`: `c6link_set_boot_hooks` (main.c `start_c6link`, onto `boot_progress_*` for `BOOT_C6`) and
  `c6link_set_net_hooks` (`wifi_service_init`, called from `start_c6link` before `c6link_start`). An unset hook does its
  default (pinned by `host_tests/c6_hooks_test.c`): no boot step shown, no Wi-Fi started, the re-attach does not wait
  for a driver. The supervisor's hooks (`c6link_set_hooks`, `START_C6_HOOKS`) are unchanged.
- **Reset settle:** `CONFIG_ESP_HOSTED_HOST_CP_RESET_SETTLE_MS=700` (the C6 answers on SDIO about 700 ms after reset, #87);
  host reset on GPIO12. `TRANSPORT_RESTART_ON_FAILURE=n`: a failed transfer posts `TRANSPORT_FAILURE`, which c6sup
  recovers from instead of aborting the P4.
- **Lost-wakeup fix:** lives in our `esp_hosted/` fork (submodule). Do not bump or replace the submodule without it.
- **Frame pools (#260):** Wi-Fi data frames (STA/AP) come from fixed pools reserved at link-up and freed at link-down
  (`esp_hosted/.../eh_host_frame_pool.c`); depths are `CONFIG_ESP_HOSTED_HOST_SDIO_TX_Q_SIZE` / `RX_Q_SIZE` (10/10 =
  31360 B). The blocks are in PSRAM (internal DMA only when PSRAM is full): in RX STREAMING mode the CPU copies every
  frame into or out of an internal DMA buffer, so no SDMMC DMA touches PSRAM (#121). Those DMA buffers (TX write buffer
  1536 B, RX staging, TX aggregate) are forced internal in `eh_host_bus_sdio.c`; a zero-copy TX frame in PSRAM is
  copied into one; PACKET mode keeps frames internal at compile time. Empty TX pool: the sender waits up to
  `TX_POOL_WAIT_MS` (5), then lwIP gets `ESP_ERR_ESP_NETIF_TX_FAILED`; empty RX pool: the frame is dropped and counted.
  RPC/HCI frames stay on the per-frame heap, PSRAM first; the received copy for lwIP goes to PSRAM. `fw c6` shows
  `c6: pool tx|rx depth .. mem psram|internal in_use .. peak .. waits .. drops .. lost ..`. At link-down the free
  blocks are freed; one still out returns through the next link session, and if it has not by the following link-down
  it is left allocated and counted in `lost` (freeing it could hand a block still being written to the next pool).
- **TCP window:** `CONFIG_LWIP_TCP_WND_DEFAULT=16384` (16 KiB), `LWIP_TCP_RECVMBOX_SIZE=16`.
- **`WIFI_PS_NONE`:** `lichen_net/wifi_service.c:152` (`wifi_ps_mode`, #146/#76). The IDF default modem sleep gives RTT spikes of
  seconds; `fw wifi ps` changes it.
- **PSRAM DMA to SDIO (#121):** SDIO transfers use buffers the DMA reads; PSRAM at 120 MHz is only kept after the DMA gate
  (`docs/psram-speed-switch.md`). Internal DMA RAM stays scarce: bench gate `fw mem`.

- **Pairing:** the P4 builds the fork through `override_path` (`firmware/main/idf_component.yml`: v3.0.10 plus the SDIO RX
  lost-wakeup fix, #60); the C6 uses registry 3.0.10 (`c6/main/idf_component.yml`). So only the P4 has the fix. The
  transport settings must match on both sides: SDIO stream mode (host `HOST_SDIO_RX_STREAMING_MODE=y`,
  `firmware/sdkconfig.defaults:91`; C6 `EH_TRANSPORT_CP_SDIO_MODE_STREAM=y`, `c6/sdkconfig.defaults:20`), frame checksum (C6
  `EH_TRANSPORT_CP_SDIO_CHECKSUM=y`, :50, the host mirrors it at runtime), peer data (host `HOST_FEAT_PEER_DATA=y`, :94; C6
  `CP_FEAT_PEER_DATA=y`, :53), SDIO slot 1, 4-bit bus and pins (`firmware/sdkconfig.defaults:78-85`) and the 40 MHz clock
  (:90, C6 `CP_SDIO_HIGH_SPEED=y`, :21). Change one side and the other in the same wave.

### Start order (main.c)
`app_main` walks the table `boot_order[]` in `firmware/main/boot_order.c` (#210) and runs one `start_*` function per
entry (`start_fns[]` in `main.c`); moving an entry moves the boot. `host_tests/boot_order_test.c` pins the rules and fails
when one breaks:
1. Storage and the SD card (`START_STORAGE`, `START_SD`, #60) come before `START_C6LINK`: the C6 image files are on
   `/storage`, and `esp_hosted_init` adds SDMMC slot 1 to the controller the card shares.
2. The IDENT hooks (`START_LORA_IDENT`: `c6link_set_ident`, `c6link_set_ident_ready`) are set before `START_C6LINK`.
3. `START_C6LINK` (`c6link_start`) comes before `START_BACKLIGHT`, `START_KEYBOARD` and `START_LORA`; `START_C6_HOOKS`
   (`c6_wire_hooks`) only after those three. The boot step enum in `lichen_boot/boot_steps.h` is a display order only.

The OS rules in the same test include `START_WATCH` (the thermal watchdog and the notices, #281) after `START_NOTICES`
and `START_SHELL`: its own step, so `start_shell` returning early without a display cannot skip it.

### 2. Peer-data channel (ESP-Hosted `eh_*_peer_data`)
| Id | Direction | Service | Registered at |
|---|---|---|---|
| `0x4C01` `LORA_MSG_ID_TO_C6` | P4 to C6 | LoRa | `c6/main/lora_service.cpp:463` |
| `0x4C02` `LORA_MSG_ID_TO_P4` | C6 to P4 | LoRa | `lora/lora.c:463` and `:634`, `lora/lora_ident.c:46` |
| `0x4C03` `I154_MSG_ID_TO_C6` | P4 to C6 | 802.15.4 scanner | reserved, not wired yet (#172) |
| `0x4C04` `I154_MSG_ID_TO_P4` | C6 to P4 | 802.15.4 scanner | reserved, not wired yet (#172) |

- A new service takes the next free id in the `0x4C0x` block and adds a row to `peer_ids` in the contract test. `i154_proto.h` also `_Static_assert`s that its ids differ from the LoRa ones.
- **Message framing:** `[version][type]` then little-endian fields. P4 to C6 types count from 1; C6 to P4 types from 0x81.
  Both images build the same headers: `shared/lora/lora_proto.h` (v4) and `shared/i154/i154_proto.h` (v1).
- **Versions:** the version byte must match exactly. LoRa v4 (RSSI sweep, #161): a v3 C6 drops every v4 message
  (`BAD_VERSION`), IDENT_REQ included, so IDENT never comes and the P4 reflashes the bundled C6 image. IDENT itself was
  new types only (no bump); an old peer answers nothing, which counts as "not proven". i154 CAPS may carry newer
  trailing fields; every other length is exact.
- **CONFIG (type 1, 14 bytes of body, 16 in all):** freq u32, bw u32, sf u8, cr u8 (5..8 = 4/5..4/8), sync u8,
  preamble u16, tx power i8 dBm. The P4 sends it on every (re)start of the link (`LORA_ACT_SEND_CONFIG`,
  `lora/lora.c:377`); the C6 starts unconfigured after its own reset. Default: 869.618 MHz, 62.5 kHz, SF7, CR 4/5, sync 0x12,
  preamble 16, 22 dBm.
- **Callback handover (0x4C02):** at boot `lora_ident` (`lora/lora_ident.c:39`, registered at :46) owns the id and takes the
  IDENT answer; `lora_start` registers `on_peer_data` (`lora/lora.c:634`) and takes it over. From then on `lora_ident`
  forwards to `lora_ident_live` (answer received by the LoRa task, `lora.c:710`). A second owner of the id would starve one
  of them.
- **After a C6 reset:** `lora_c6_lost` / `lora_c6_back` (`lora/lora.c:680`, `:699`) stop and restart the polling and
  re-send CONFIG, then `RX_START`.
- **Radio switch (#108, #222):** off sends `RX_STOP` and holds CONFIG + `RX_START` back until on, while `lora_send`
  refuses. The state lives in `lora/lora_switch.c` (`lora_set_enabled`, `lora_enabled`); `fw lora on|off` hands the
  request to the owner main.c injects (`lora_set_switch_owner`, the OS switch `lichen_radio_set`), never to a header
  above L2. No owner: the console switches LoRa alone, not saved (`host_tests/lora_switch_test.c`).

### 3. C6 boot, flash and IDENT
- **Files** in `/storage/c6` (`C6_IMAGE_DIR`, `c6link/c6_paths.h`): `bootloader.bin`, `partitions.bin`, `firmware.bin`,
  each with a `.md5` (`c6link/c6_firmware.c:214`). A `.md5` that does not describe its `.bin` is refused
  (`c6_md5_matches`). **`ota-c6` order: the `.bin` first, then its `.md5`**, all six files each time; the C6 stays
  untouched if it fails, so repeat before any reboot.
- **Plan** (`c6_boot_plan`): a warm software restart with an unchanged bundle key skips the ROM check; everything else,
  a forced reflash (NVS `c6_force`, `c6link.c:48`), a `c6_verify` flag or a trial image, checks fully. NVS keys:
  `c6_ok_key` (bundle key `md5,md5,md5,size,size,size`, stored only after attach and IDENT MATCH), `c6_verify`
  (`c6link.c:129`), `c6_rt_restart` (`c6_sup.c:45`), `c6_force`. A skip that loses its bet (no attach, IDENT not
  proven) restarts once with the verify flag, only if that flag persisted (`c6_restart_after_attach`).
- **Bundle changes:** a push into `/storage/c6` over Wi-Fi (`[c6]` file) calls `c6link_bundle_changed`
  (`ota/ota_server.c:405-408`) before touching the file, so the stored key is forgotten and the next boot checks fully.
  Every writer of that directory must do the same.
- **P4 OTA validity needs the C6:** a pending image is marked valid only once, after `OTA_VALID_AFTER_S`, the C6 link
  is up, no PSRAM trial is live and, on a boot with a display, the shell is up (`ota/ota_valid.c` `ota_trial_step` and
  `ota_trial_run`, the loop `main.c` `ota_trial_wait` drives). Only a deliberately headless boot (`fw noscreen`) is
  proven without the shell (#293); a display that failed to start never proves the image, so the next reset rolls it
  back. While the C6 link is down the trial keeps watching and rolls back at `OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S`
  (#36, #295). A broken C6 link rolls back every P4 OTA; this is why `ota-c6` goes first. The trial state has one owner,
  `ota/ota_trial.c` (#285): everyone else asks `ota_image_trial_pending` (the shell `lichen_sys_update_on_trial`, the
  layers below ota through a hook main binds).
- **Restarts on trial** (#304): a restart while the image is on trial rolls it back, so `system_restart` (an automatic
  restart) first waits, polled once a second, on the gate main binds (`system_restart_set_trial_gate`; the rule is
  `ota_valid.c` `restart_gate_decide`): until the valid mark, the trial loop's end (any end), `OTA_RESTART_GATE_MAX_S`
  (120 s) of uptime or a power off. `system_restart_asked` (fw reboot, /reboot, the Power menu, factory resets, the
  trial's own rollback, a good install's reboot) never waits; fw reboot and /reboot say the restart goes back to the
  previous version. Callers that must not block ask `system_restart_would_wait`: the update screen holds its restart
  ("Waiting for this version to be confirmed"), MeshCore's remote reboot is refused. A bare `esp_restart` on trial
  only logs a warning.
- **IDENT** (#36): after attach the P4 asks the running C6 app for its ELF sha256 (32 bytes, `LORA_MSG_IDENT`) and
  compares with the bundled `firmware.bin` (`c6_ident_check`; wait `C6_IDENT_WAIT_MS` 2000, `c6link.c:133`). Only MATCH
  proves the C6; NO_ANSWER, MISMATCH, NO_EXPECTED, NOT_ASKED are all "not proven". After a runtime re-attach IDENT is
  asked again (`c6_sup.c:331`) once LoRa is back. `fw c6` shows the result.
- **Flasher** (`c6link/c6_flasher.c`): UART1 on GPIO15/16, reset GPIO12 (EN), boot strap GPIO13; it releases the C6 into its
  application before ESP-Hosted may talk to it.

### 4. Hardware couplings
- **C6 reset resets the keyboard:** GPIO12 (C6 EN) also resets the TCA8418. The C6 starts before the keyboard
  (`START_C6LINK`, then `START_KEYBOARD`, then `START_LORA` in `firmware/main/boot_order.c`; `START_C6_HOOKS` only after
  all three, pinned by `host_tests/boot_order_test.c`). A later C6 reset runs `keyboard_reinit`, `backlight_reapply`, `lora_c6_back` in that order
  (`main.c` `c6_up`).
- **Backlights go through the C6:** UART1 text command `\nBL <display> <keyboard>\n`
  (`board/backlight_level.c` `backlight_cmd_format`, sent from `board/backlight.c`). With the C6 down brightness does not
  change, and Settings says so.
- **R16 / GPIO5 hold (#12, #39):** stock badges have no R16 so GPIO5 is not on `DISPLAY_BL`; with the bridge fitted the P4
  holds the backlight while the C6 is in reset or its ROM (`board/r16_hold.c`; flasher order in `c6_flasher.c:106-120`: EN
  low, GPIO5 back to input, then EN rises). Power off drives backlights off (`backlight_power_off` in `main.c` `power_finish`), then `c6link_hold_in_reset` (same function, defined at `c6link.c:64`). The hold starts once the ROM answered, not before.
- **C6 bootloader and backlight (#39):** there is no C6 bootloader hook (dropped by decision). The P4 rewrites the C6
  bootloader at 0x0 when its MD5 differs from the bundle's (`c6_flasher.c:68`); a broken C6 bootloader means USB-only
  recovery. Until the first `BL` line arrives (`c6/main/backlight_cmd.c`) the C6 backlights run at their boot levels
  (`c6/main/app_main.c`, `backlights_start`), so a P4 that never sends one leaves the panel at the C6 default.
- **UART1 is shared** by the flasher, `fw c6log` and the backlight commands (`board/backlight.c` `BL_UART`); the
  backlight owns it and lends it, one borrower at a time (#250; pure state in `board/uart_lend.c`,
  `host_tests/uart_lend_test.c`). The flasher takes an exclusive lend (`backlight_uart_lend(true)`: the backlight's
  driver is deleted, from `esp_loader_init_serial` to `esp_loader_deinit`); meanwhile the backlight never installs or
  writes, and the newest levels asked for go out on `backlight_uart_return` (a restart in that window cannot blank).
  `c6_log_console` takes a shared lend (`backlight_uart_lend(false)`): it reads through the backlight's driver, which
  the lend installs if needed, never installs or deletes one, and BL lines keep going out. `c6link_start` registers
  `c6log` only after the flash check, so the two lends cannot meet. The lend is a flag under `uart_lock`, not the lock
  held, so a UI send never blocks for the seconds a flash takes. `backlight_start` still comes after `c6link_start`
  and sends the saved levels.
- **`esp_hosted_init` failure is sticky** until the next boot (`c6_sup.c:279`); only a failed connect is retried by a
  re-attach.

### 5. Watchdog, recovery, Bluetooth
- **Watchdog:** the C6 LoRa service sends STATUS (uptime, heap, stack) about every 60 s (`c6/main/lora_service.cpp:58`);
  a drop in uptime means it restarted. `c6_watch` (pure) turns signals into actions: hard (SDIO `TRANSPORT_FAILURE`,
  keyboard reset hook) or soft (three failed LoRa sends then a probe). Up to 3 re-attaches (2 s then 10 s apart), then
  a slow retry every 5 min, one offered restart of the badge (never on a trial image). Down hooks stop users of the
  link; up hooks restore keyboard, backlight, LoRa.
- **BT VHCI:** the C6 image builds the BLE controller with HCI over VHCI (`c6/sdkconfig.defaults`), but the P4 starts no
  host stack: Bluetooth is off after boot (#197). Bench gate: `fw airplane` shows `bluetooth off`.

## Kernel and tick

- FreeRTOS: Espressif's dual-core fork, V10.5.1 (ESP-IDF 6.1). The upstream SMP kernel (V11.1.0,
  `CONFIG_FREERTOS_SMP`) can't be selected for the P4: it depends on `IDF_TARGET_ESP32` and is marked "feature under
  development". Revisit when Espressif supports it on the P4.
- Tick: **1000 Hz** on both chips (`CONFIG_FREERTOS_HZ=1000`, #69): 1 ms sleeps for the mesh loop, the keyboard and
  LVGL, at under 0.5% CPU.

## Cores on the P4

The rule: **core 1 draws, core 0 talks to the world.**

| Core | Runs |
|---|---|
| 0, connectivity and services | ESP-Hosted SDIO tasks (prio 22), esp_timer (22), lwIP tcpip (18, pinned), LoRa link task (3), apps' background tasks (`lichen_task_start` classes BACKGROUND, SERVICE, JOB, URGENT), OTA server (5), console (5), keyboard polling (4), main |
| 1, screen | the LVGL task (pinned: `board/display.c`, `task_affinity = 1`), every app's screen code, which runs in LVGL's context, and an app's main loop (`LICHEN_PRIO_APP_LOOP`: MeshCore's mesh task) |

- The screen never stutters because of signature checks, Wi-Fi traffic or OTA decompression.
- The radio path (C6 → LoRa task → queue → MeshCore) stays on one core.
- The cost: during a large Wi-Fi push MeshCore gets less time; its queue absorbs that, because packets arrive seconds
  apart.
- Apps never call LVGL from core 0; they hand data to their screen code through a copy taken under a lock (as
  `mesh_*_snapshot` does).

## Using the hardware better

- **PPA (2D accelerator):** let it do the 90° rotation instead of LVGL's software rotate, freeing core 1. An
  experiment: the earlier attempt with the DSI ISR / DMA2D IRAM options broke esp_lvgl_port 2.9.
- **Hardware SHA, AES and ECC exist, but MeshCore's crypto is Ed25519/X25519,** which they don't cover. Not worth it
  now.
- **Memory:** large state in PSRAM, internal DMA-capable RAM at 150 KB or more free.

## Apps

- Built into the firmware, each a component with a small C interface: a tile, screens, a settings page, a background
  part. What an app may use, what it must not, and the linker wraps that hold the line: [app-api.md](app-api.md)
  (#84).
- **No downloadable apps for now.** Loadable native apps (ELF) were BadgeVMS's biggest source of trouble (per-app
  heaps and stacks, wrapped functions, an export table). A sandboxed script runtime (WebAssembly through WAMR, #78)
  is the safe way to open up later and could be one app running scripts through the same interface (v2: WAMR, below).
- Others can already write an app as a component and build their own Lichen.
- Notifications (#62) are an OS service in the app API: `lichen_notify(source, level, title, body, flags)` from any
  task. Lichen shows a toast that never takes the focus, the unread count on the app's tile and in the status bar, and
  keeps a history of 48 in `/storage/lichen/notify.bin` (the Notifications tile, the cloud key). The user switches
  sources on or off and sets do-not-disturb in Settings › Notifications. Lichen raises its own notices too (a crash,
  the PSRAM check, the clock, an update, the C6 coming back, the SD card, a sensor, low storage): `lichen_os/sys_notices.c`,
  an OS service like the thermal watchdog below (#281).
- The thermal watchdog (#10, `lichen_os/thermal.c`, rules in `thermal_rules.c`) and the notices are OS services, not
  shell parts (#281): main starts both in their own boot step `START_WATCH` (`lichen_watch_start`, `lichen_os.h`)
  whether or not a display came up, so a badge without a screen still warns and powers off when too hot. What they
  need from above comes in as hooks (`lichen_watch_hooks_t`): "home is shown", the shell's warning page, the update
  result and the power off, which is the Power app's goodbye path when the shell runs and main's headless path
  (`system_save_before_off`, the SD card unmounted with a bound, then `lichen_sys_power_off_now`) otherwise
  (`main/power_route.c` decides, host-tested; `fw poweroff` takes the same route, #292; before the shell has started
  a request takes the headless route). Once a power off is asked, every restart (`fw reboot`, an update, a bare
  `esp_restart`) ends in the power off after its saves (`system_power_off_requested`). A thermal shutdown sheds
  nothing first, turns a Power-app restart under way into a power off, and has a backstop: 30 s after it was decided
  the badge is off, saved or not. Without a display the notices wait until every boot step ran. The shell reads the
  watchdog through the gate
  (`lichen_sys_thermal.h`). `fw thermal` says whether the watchdog task runs; `fw noscreen` makes the next boot skip
  the display start (one boot only) to prove it.
- The load shed holds Wi-Fi and LoRa off through the radio service (#282, `lichen_sys_radio.h`): a hold has a reason
  (thermal), lives in RAM only and never writes the person's saved switches, so a boot starts without one and the
  radios come back as the person set them when the reason ends. While held a radio reads off; switching it on
  overrides the hold for that radio, switching it off is saved as usual. `fw airplane` shows `held off (hot)`.
  Its headers are the gate's (`lichen_api/lichen_notify.h` for apps, `notify_os.h` for the shell, `notify_entry.h`),
  implemented by the `lichen_notify` service (L3) like `lichen_os` implements the rest (#245). The drivers below it
  never call it: lichen_os (`os_bind.c`) binds the status LED's `led_set_dnd`/`led_set_unread`, which notify calls
  on every do-not-disturb change and history change, and `haptics_notice` as the vibrate feedback the shell fires on
  the LVGL task for a toast. lichen_boot's factory reset is an L3 service like notify and could call it, but it may not
  include the gate's `lichen_notify.h` (L3 may not use L4), so its notices go through a hook bound the same way.
- Home's tile order (#189) is a display order only: the app's registry index stays its identity (screen ids, the nav
  stack, shortcuts, badges, notifications). Stored as `badge_fw/home_order`, text `"1:settings,about,files,..."`
  (version, then app ids; `lichen_api/tile_order.h`): unknown and duplicate ids are skipped, apps not named follow in
  registration order (a new app goes to the end), a corrupt or other-version value gives the registration order.
  Written only on a real move; back at the registration order the key is erased. A factory reset erases it; it is
  not in the settings backup (the backup's text values are capped at 63 characters).
- The six shape keys between Esc and Backspace are OS shortcuts (#79), the same on every screen: ■ home, ▲ messages
  (the app with `open_messages`, MeshCore's Chat), ✕ back to the previous app, ● search (the screen's
  `shell_on_search` hook: list filters, a chat), ☁ notifications, ◆ the Power app. The shell hears them as raw key
  events before any focus; apps never get them as keys or text. Not while a dialog, the Keyboard Test, first-run
  setup, an update or a power-down is on screen. Bench: `fw keys send square|triangle|cross|circle|cloud|diamond`.

## Layers

Status: **in force (OS v2, #216)**. `tools/check_layers.sh` reads `tools/layers.conf` and runs at the end of
`build.sh build`; the layer baseline (`tools/layers-baseline.txt`, #218) is **0** since the W9 close-out (#237), so
any new violation fails the build. The two-chip protocol is a separate contract: see "Two-chip contract" in this file.

| Layer | What | Component(s) in `tools/layers.conf` |
|---|---|---|
| L0 platform | IDF-level helpers every layer may use: console, PSRAM guard, random, assets, NVS init and encryption | `fw_console`, `psram_guard`, `lichen_random`, `assets`, `lichen_nvs` |
| L1 board/HAL | pins, buses, panel, keyboard chip, storage drive; no NVS of its own (#224: settings arrive through a bound getter/setter) | `board`, `esp_lcd_st7703`, `esp_tca8418`, `storage` |
| L2 drivers | radios (LoRa, the C6 link), SD card, sensors, LED, haptics, battery, audio | `sdcard`, `sensors`, `lora`, `c6link`, `lichen_led`, `lichen_haptics`, `battery`, `lichen_audio` |
| L3 OS services | the gate's implementations and the services behind it | `lichen_os` (settings, fs, crash log, radio, task and memory books, fonts, the UI service, the thermal watchdog and Lichen's notices (#281); implements the gate's headers, #220), `lichen_notify` (implements the notification headers, #245), `lichen_clock`, `lichen_net`, `lichen_boot`, `lichen_mdns`, `ota`, `crash` |
| L4 the gate | headers and their pure logic, the gate's widgets, one ops table; no hardware dependencies (may use L0 and itself only) | `lichen_api` |
| L5 system | the shell and the system screens, a privileged client of the gate | `lichen_shell` |
| L6 apps | the plain apps and MeshCore, a closed profile | `lichen_apps` (#180), `mesh` |
| main | the composition root, may use every layer | `main` |

`main` should only call init, start and stop of the layers; `layers.conf` lets it depend on every layer and still
catches its owner-only symbol uses. `esp_hosted` is a vendored IDF component, not a repo component: it has no layer
line and is reached only through `c6link` and `lora`.

**Allowed calls (a layer calls itself and the layers below it, with exceptions):**

| From \ To | L0 | L1 | L2 | L3 | L4 | L5 |
|---|---|---|---|---|---|---|
| L1 board | yes | | | | | |
| L2 drivers | yes | yes | | | | |
| L3 services | yes | yes | yes | | | |
| L4 gate | yes | | | ops table only | | |
| L5 shell | yes | no | no | no | yes (both surfaces) | |
| L6 apps | yes (plain libs) | no | no | no | app surface only | no |

(`layer-may` lines in `layers.conf`: L4 may use L0 and L4; L5 L0, L4 and L5; L6 L4 and L6, plus the named `peer` lines.)

- **Callbacks go up, calls go down.** A lower layer never names a higher one. When it must tell a higher layer
  something (the radio switch, a C6 event, a UI request, a notification's feedback) the higher layer registers a
  callback or fills an ops table at boot (the Flipper "record" and Zephyr driver pattern). Examples: c6link's boot and
  net hooks (`c6link/c6_hooks.h`), board's settings getter/setter, the shell's `lichen_ui_ops_t`.
- **The shell reaches hardware and shared state only through the system surface**, never through drivers or `board`
  directly. `lvgl_port_lock`, `nvs_open*`, `heap_caps_*`, `xTaskCreate*` and `lv_group_set_default` are owner-only
  symbols with the owners listed per symbol in `layers.conf` (rule C).

### The gate: its headers

The gate **is its headers**: every function a `lichen_api` header declares is the gate. The implementers are `lichen_os`
(L3, #220) and `lichen_notify` (L3, #245); `layers.conf` gives each a `peer ... lichen_api` line, and a call to such a
function counts as a call to the gate, not to the service (`GATE_IMPL` in `tools/check_layers.py`). The gate itself
never depends on the implementers. `tools/gate_manifest.txt` lists every declared function with its capability and
thread rule and must match the headers on every build (`check_layers.sh --gen-manifest` regenerates it). Where a header
needs a type that lives in a lower component, the gate repeats it and `lichen_os` (`lichen_sys_pure_view.c` and the
like) asserts that the copies stay equal.

The shell's navigation, dialog, station clock and raw keys (`lichen_ui.h`, `lichen_keys.h`) are defined by the gate
itself: `lichen_ui_ops.c` forwards them to the `lichen_ui_ops_t` table the shell sets in `shell_boot_screen` and
`shell_start`, with a defined "not up" answer before (#221), so no caller links upward to the shell.

**Two surfaces** (per-header lines in [app-api.md](app-api.md)):
- **App surface:** `lichen_*.h`, for L6 (the shell uses it too).
- **System surface:** `lichen_sys_*.h`, for L5 and `main`. `forbid L6 lichen_sys_*` in `layers.conf` makes any app use a
  rule C violation. The headers: `lichen_sys_ui.h` (the LVGL lock and UI calls, #225), `lichen_sys_display.h` (backlight
  levels, #226), `lichen_sys_power.h` (screen off for power-off, #226), `lichen_sys_storage.h` (drive and SD card, #227),
  `lichen_sys_update.h` (Wi-Fi update server setting, #227), `lichen_sys_crash.h` (crash-recovered flag, #227),
  `lichen_sys_factory.h` (factory reset, #227), `lichen_sys_mem.h` (heap figures, #228), and from #244
  `lichen_sys_clock.h`, `lichen_sys_net.h` (Wi-Fi and mDNS), `lichen_sys_boot.h` (boot progress and the console log
  tap), `lichen_sys_keys.h` (keyboard hooks) and `lichen_sys_led.h` (status LED setting).
- No syscall trap and no export table: the P4 runs everything in M-mode with no configurable MPU, and U-mode/TEE is
  not available on it, so a trap would protect nothing and cost a context switch.

**Capabilities (#229, still to come):** the manifest's capability column is a per-header seed (`-` = none). Per-app
declarations in `layers.conf`, a build-time `nm` check (rule D) and a run-time refusal come with #229, and the
identity, lifecycle and crash-attribution waves (#230-#233) after the MVP. Capabilities are an accident and audit
guard, not a security boundary: apps are compiled in and share the address space.

### Services behind the gate

- **The UI service owns the LVGL lock** (#225, `lichen_os/lichen_ui_call.c`): the shell and main take it through
  `lichen_sys_ui_lock`/`_unlock` and hand work over with `lichen_sys_ui_call`; apps use `lichen_ui_call`; builders set
  the default group only through `lichen_ui_with_group`. Calls for the LVGL task wait in a 16-entry FIFO that one
  `lv_async_call` drains per batch (a burst past it falls back to one async call each). Order is FIFO: since #286
  nothing outside the UI service calls `lv_async_call` (`tools/layers.conf` owner-only), and `lichen_sys_ui_cancel`
  takes back a queued call whose argument is going (a menu row's delete). `board`
  keeps the port lock for the port setup at boot (display, keyboard) only: it sits below `lichen_os`.
- **The lock-owner check** (`CONFIG_LICHEN_UI_LOCK_CHECK_MODE`: off, **count (default)**, assert): the linker wraps
  LVGL's allocator and `lv_inv_area`; once `main.c` arms it after the keyboard is set up, LVGL touched by a task that
  is neither the LVGL task nor inside `lichen_sys_ui_lock` is a hit. In count mode the hits are counted and the first
  per call site is logged; read them with `fw ui lockcheck` (`lockcheck reset` clears). The MVP bench wants 0 hits
  (`docs/mvp-bench.md`). Assert mode aborts instead. This project has no build with assertions off, so the chosen mode
  is in every image.
- **The task book and the memory book** (#181, `lichen_task.h`, `lichen_mem.h`; pure parts `task_book.c`, `mem_book.c`):
  every task started through `lichen_task_start` is booked under its app (32 slots; a priority class maps to a core and
  a priority, the app loop sits on core 1 next to LVGL), every `lichen_alloc` is counted per app (25 owners; a 64 KB
  internal-RAM quota, bulk goes to PSRAM). A task's stack is internal by default; `lichen_task_start_in` puts it in LP RAM
  (low-rate tasks that must stay internal-capable but never need DMA: sys_notices and thermal, #269) or PSRAM, falling
  back to internal RAM when the region is short. `xTaskCreate*` and `heap_caps_*` are owner-only symbols, so apps and the
  shell cannot bypass the books. Read them with `fw tasks` and `fw mem`.
- **Notifications reach the LED and the haptics by a push from above** (#245): the drivers (L2) never call the
  notification service. `lichen_os` (`os_bind.c`) binds the status LED's `led_set_dnd`/`led_set_unread`, which
  `lichen_notify` calls on every do-not-disturb and history change, and `haptics_notice` as the vibrate feedback the
  shell fires on the LVGL task for a toast. `lichen_sys_led.h` is Settings' side of the LED.
- **The console table** (`fw_console`, `MAX_COMMANDS` = 96): `console_register` refuses past it and logs "no room for
  command ..."; `fw help` ends with "N of 96 commands registered". `tools/check_console_slots.py` runs in
  `build.sh build` and fails when the image's `console_register` calls pass 80 % of the table (#243). Raise
  `MAX_COMMANDS` (12 B of .bss per slot) before the guard trips, not after.

### Enforcement

`tools/check_layers.sh` with `tools/layers.conf` (#218; it absorbs `tools/check_app_boundary.sh`'s rules and runs at the
end of `build.sh build`): A, `REQUIRES` rules; B, includes; C, symbol ownership through `nm`, which catches link-time
upward edges; D, capabilities (#229, not in the checker yet). Rule C counts per object file, A per edge, B per include
line. The app profile (L6) is a closed set of libraries, headers and forbidden symbols that no baseline can excuse.

- **The ratchet is at 0.** `tools/layers-baseline.txt` holds no entries (only its header comment). A violation fails as
  NEW or WORSE; a baseline entry added against the merge-base copy fails as GREW ("the baseline only shrinks"); STALE
  catches an entry that was fixed and not removed. Do not add entries: fix the edge, give the symbol an owner in
  `layers.conf` with a reason, or add a hook.
- **The escape hatch is `BADGE_LAYERS_ALLOW_GROW=<issue number>`**, a one-off for NEW rule-C entries only: an entry whose
  mark names `#<issue>` may be added, as moved debt that the named later issue removes (the move cannot be done without
  a gate function that issue adds). It prints a WARNING, never applies to raised counts, and is never set in
  `build.sh` or a hook.
- Apps (`mesh`, `lichen_apps`) keep named `peer` lines in `layers.conf` (`lichen_clock`, `lichen_random`, `lora`,
  `fw_console`, `psram_guard`), each narrowed to the headers app-api.md names by a `peer-headers` line (#283):
  reasoned exceptions that the app surface may still remove.
- **Where things sit:** the shell and the system screens (Settings, About, Files, Power, Update...) are L5; MeshCore and
  the plain apps are L6; the C6 is a second chip behind the c6link driver (L2) and its protocol is the two-chip
  contract, not a layer.

**Fault isolation is detection and containment, not protection** (waves L-D and L-E, after the MVP): per-app grants
(memory, PSRAM quota, booked timers) freed whole on stop; cooperative stop with a deadline (foreign tasks are never
deleted); crash attribution through RTC no-init and a boot-loop breaker (2 crashes attributed to one app in 3 boots
switch its background off and notify); task-watchdog heartbeats; quotas that refuse rather than exhaust.

## Console flood scope (#290)

Every flood a console line starts (`fw mesh send`, `dm`, `remote <who> login|status|cli|settings`, `telemetry`,
`discover`, `history <who> on`, `fw advert flood`) goes out under the **console scope**, a region of its own; the
screens keep the default scope (#163). The origin travels with the request (outbox entry, trace and remote request,
history slot) and `BadgeMesh`'s `sendFloodScoped` overrides pick the console key while MyMesh handles that frame.

- Kept in MeshCore's directory (`/console_scope`, written through a temp file), so the factory reset that clears the
  default scope clears it too. `fw mesh scope` shows both; `fw mesh scope console <region> YES` sets it.
- Not set: the badge refuses console floods (and drops one that still reaches the radio), never sends them unscoped.
- History polls a console line switched on keep that origin across restarts (a `c` on their setting entry). When the
  console scope changes, or is gone after a reset, they are switched off and saved: they were meant for the old region.
- `tools/badge.sh` reads `fw mesh scope` through the same path before each line that can flood and refuses unless the
  one exact status line names the allow file's `scope <region>` line (`tools/onair_guard.sh`).

## Channel flood scope (#291)

A channel can carry a **flood scope of its own**: a message sent on it from the screens floods in that region instead
of the default scope (#163); other channels and every other flood keep the default. A console line's send keeps the
console scope regardless (#290 wins). One chooser decides, `mesh_scope_choose` (console > the channel's own > default),
called in `BadgeMesh`'s `GroupChannel` override of `sendFloodScoped`.

- Badge-local (MeshCore's share format has no room for it): `/channel_scopes` in MeshCore's directory, written through
  a temp file, one line per channel keyed by the first 8 bytes of SHA-256 of the channel's secret (two channels with
  the same secret share one scope). A rename keeps it; removing the channel clears it, after the channel is removed
  and saved; the factory reset's format clears it with the default and console scopes.
- Damage fails narrow: a line whose id reads but whose region does not (or a last line a cut write left without its
  line end) is a tombstone, and that channel's screen sends are refused ("flood scope damaged: set it again") until
  the scope is set again; the details row and `fw mesh scope` show it. Only the temp file there (a cut between the
  remove and the rename) promotes it. A file that is there but does not read whole (will not open, 0 bytes, a line
  without a readable id, a last line without its line end) is unreadable: every channel's screen sends are refused
  ("flood scopes unreadable: set them again") until the next successful save. No file at all is the first start (no
  scopes). A start drops the lines whose channel is gone (never while no channel loaded) and writes nothing.
- The senders check without the mesh lock against a copy in `mesh_cscope.c`: FNV-1a hashes of the damaged channels'
  names (a collision can only refuse a healthy channel) and the unreadable flag, renewed after every set, add, remove,
  rename and load; the outbox and the radio-side override refuse again under the lock.
- Set and shown in the channel details screen (row "Flood scope": the region, or "Default (<name>)"); a region asks
  first in the careful dialog, going back to the default does not. `fw mesh scope` lists the channels with their own
  scope after its status line (`channel <name>: scope <region>`, lines `tools/onair_guard.sh` never takes for it).

## Stress run (#278)

`fw stress [1-30 | stop | status]` (default 10 min; console only, the badge has no tile for it) loads the badge for
minutes and reports how deep memory and stacks went (the numbers #268 needs). `lichen_shell/stress_workers.c` runs it
on OS tasks (`lichen_task_start_in`, internal stacks): stress-mon (SERVICE, 4 KB), stress-dma and stress-sd
(BACKGROUND, 6 KB), stress-fs (BACKGROUND, 4 KB), stress-lat (SERVICE, 2 KB). Workers: DMA gate rounds
(`psram_dma_quick_round`: a busy gate is counted as skipped, never as a pass) and a cache-flushed 1 MB PSRAM copy
checked against its pattern; 64 KB write/read/compare/delete in `/sd/lichen/stress` (paced so a run writes at most
64 MB); a 512 B file on `/storage` every 10 s; a 10 ms latency probe. The run's screen (`app_stress.c`, see
`docs/shell-design.md`) is the UI load: it redraws itself every 100 ms. LoRa is never used: nothing is transmitted.
Wi-Fi load comes from the host: `tools/stress_host.sh <host> <minutes>` pulls `/log` and `/screenshot` in a loop,
retries a 503 with backoff and exits non-zero on the first other answer that is not 200.

- **Keys**: the physical keys are locked for the run with the #190 lock (`ui_remote_hold`; the monitor counts as
  console activity, so the lock's timeout never ends it). Esc held 3 s takes them back and stops the run (`esc`).
  They are free again at the end, on every exit path.
- **Refused** while a run is on, during an update or power off, during the 120 MHz PSRAM trial, with the C6 link not
  up, at a thermal warning, or under 48 KB free DMA RAM or 8 MB free PSRAM. No card: the SD worker is skipped and the
  start line says so. The badge cannot read its battery (Settings > Battery), so there is no battery interlock.
- **Stops** at the first failure (`stress_calc.c`, in this order): a file or copy read back different (`mismatch`), DMA
  bad bytes or errors (`dma_bad`), a failed heap integrity check (`heap`; internal every sample, PSRAM every minute and
  at the end), a worker without its buffers (`alloc`), a `fw ui lockcheck` hit during the run (`lockcheck`; a reset
  during the run counts from the reset), the probe over 250 ms late (`latency`), DMA free under 16 KB in a sample no
  DMA gate round overlapped (`dma_floor`), a stack whose high-water fell under 256 B during the run (`stack`; a task
  already under it at the start fails only after falling 32 B more), workers still running 60 s after the stop
  (`hung`; the next run waits until they returned). Without failing: Esc held, `fw stress stop`, power off, an update,
  the 120 MHz trial starting (`trial`: its gate waits up to 10 s for a busy engine, the run gives it up within a
  second), a thermal warning (`hot`), the C6 link not up (`c6`). The test files go on every exit path, leftovers of a
  cut run at the next start.
- **Lifetimes**: workers and `stress_stop` reach the monitor only through two semaphores that live for good, never its
  task handle; the monitor clears the run flag only after every worker returned, so a new run never resets state a
  worker of the last one still uses. The per-task stack snapshots live in PSRAM for the run only.
- **Lines**: one path, `ESP_LOG` with the tag `stress` (UART and the log store alike; the console only acknowledges
  with `stress started ...`, `stress refused: ...`, `stress status: ...`, which never contain `stress:`). Pinned by
  `host_tests/stress_calc_test.c`:
  - I `stress: start <m> min workers dma,sd,fs,ui,lat` (without a card `workers dma,fs,ui,lat sd skipped: no card`);
    W `stress: sd: skipped, no card`, `stress: fs: skipped, no drive`, `stress: sd: skipped, card gone`,
    `stress: dma: skipped, gate busy`
  - I every 10 s `stress: t=<s>s dma free=<B> min=<B> largest=<B> int free=<B> min=<B> largest=<B> psram=<B> pool
    peak=<n> drop=<n> lat=<ms>ms ok` (the reason instead of `ok` once the run stopped)
  - E `stress: STOP <reason> at <s>s` for a failure, W for esc, stop, poweroff, update, trial, hot, c6; E
    `stress: stack low <task> <free>/<size>`, `stress: dma: <n> bad bytes, <n> errors`, `stress: <worker>: read back
    different`, `stress: <n> worker(s) hung`; W `stress: sd|fs: file error <errno>`
  - the report: `stress: done <s>s PASS|FAIL` (I, E for FAIL), `stress: min ...` (the lowest free figures, the
    highest peak, drops and lateness), `stress: stack <task> <free>/<size>` per task (`?` for a task the OS did not
    book), `stress: errors dma_bad=.. dma_busy=.. mismatch=.. alloc=.. heap=.. lock=.. sd_io=.. fs_io=..`,
    `stress: work dma=.. copies=.. sd=.. sd_kb=.. fs=.. frames=..`.

## Lessons from BadgeVMS

Kept:
1. **Internal DMA-capable RAM is the scarce resource** (#56, #58, #60): keep the reserve, put big things in PSRAM.
2. **A C6 reset also resets the keyboard.** Starting the C6 comes before keyboard init. Any later C6 reset is a system
   event that re-initialises the keyboard and the LoRa link (protocol version, CONFIG).
3. **The backlights run through the C6:** with the C6 down, brightness doesn't change. Settings › Display must say so
   rather than pretend.
4. **ESP-Hosted:** no auto-init (it races the C6 flasher), keep the lost-wakeup fix (our esp_hosted fork), TCP window
   16 KiB.
5. **A new image is valid only after it has run properly** (45 s with UI and C6 up): rollback stays.

Not kept: foreign ELF apps with their own heaps, MicroPython as the app layer, the launcher process model.

OS v2 (#216) on the same lessons:
- **No U-mode on the P4:** it runs everything in M-mode, PMP gives W^X only and ESP-TEE/U-mode is not available, so a
  syscall trap would add a context switch and protect nothing. The gate is a header API plus an ops table (see Layers).
- **BadgeVMS's per-process PSRAM remap is studied, not adopted:** a cost on every switch, DMA and LVGL cannot cross it,
  and its own README says processes are isolated from each other but not from the OS.
- **WAMR (WebAssembly) is the future sandbox,** the only real isolation the P4 allows: one built-in "Scripts" app
  hosting modules, each with its own capabilities (#78 probe, not in v2). It replaces MicroPython as the candidate.
- Concepts only: no BadgeVMS code, and none ever in `c6/` or `shared/`.

## Flashing on the bench

- P4 firmware over USB without touching `/storage` (MeshCore identity, contacts, channels):
  `BADGE_EXTRA_PORTS=/dev/ttyUSB1 tools/badge.sh flash-fw /dev/ttyUSB1` (keeps `/storage` by default; `--wipe-storage` is the first-install form).
- The C6 image is a file in `/storage/c6`. A full USB flash replaces `/storage` and loses the identity, so C6 updates
  go as two files over Wi-Fi (`firmware.bin`, then its `.md5`) before the P4 flash.
