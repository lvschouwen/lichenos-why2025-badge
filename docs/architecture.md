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

**Layer rule:** `c6link`, `lora` and `esp_hosted` are L2 drivers that own the link; services reach the C6 only through
them (`tools/layers.conf` enforces it: the `esp_hosted_*` and `eh_*` symbols are owner-only for `c6link` and `lora`). The C6 image (`c6/`, Apache-2.0) stays outside the P4 layer model and
is not refactored by v2. A C6-side change in a v2 wave needs `ota-c6` before the P4 image.

### 1. ESP-Hosted Wi-Fi over SDIO
- **No auto-init:** `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n` (`firmware/sdkconfig.defaults`). The C6
  flasher must release the C6 first; auto-init races it into a boot loop. Init order in `c6link_start`
  (`c6link/c6link.c:1523`): flasher check (or skip), `hold_c6_boot(1)` (:1558), `esp_hosted_init` under the SDMMC lock
  (:1561, the SD card shares the controller, #60), `esp_hosted_connect_to_slave`, IDENT, then `start_wifi`. A runtime
  re-attach repeats deinit, init, connect in `c6link/c6_sup.c:289`.
- **Reset settle:** `CONFIG_ESP_HOSTED_HOST_CP_RESET_SETTLE_MS=700` (the C6 answers on SDIO about 700 ms after reset, #87);
  host reset on GPIO12. `TRANSPORT_RESTART_ON_FAILURE=n`: a failed transfer posts `TRANSPORT_FAILURE`, which c6sup
  recovers from instead of aborting the P4.
- **Lost-wakeup fix:** lives in our `esp_hosted/` fork (submodule). Do not bump or replace the submodule without it.
- **TCP window:** `CONFIG_LWIP_TCP_WND_DEFAULT=16384` (16 KiB), `LWIP_TCP_RECVMBOX_SIZE=16`.
- **`WIFI_PS_NONE`:** `c6link/c6link.c:173` (`wifi_ps_mode`, #146/#76). The IDF default modem sleep gives RTT spikes of
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

### 2. Peer-data channel (ESP-Hosted `eh_*_peer_data`)
| Id | Direction | Service | Registered at |
|---|---|---|---|
| `0x4C01` `LORA_MSG_ID_TO_C6` | P4 to C6 | LoRa | `c6/main/lora_service.cpp:463` |
| `0x4C02` `LORA_MSG_ID_TO_P4` | C6 to P4 | LoRa | `lora/lora.c:464` and `:635`, `lora/lora_ident.c:46` |
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
  `lora/lora.c:378`); the C6 starts unconfigured after its own reset. Default: 869.618 MHz, 62.5 kHz, SF7, CR 4/5, sync 0x12,
  preamble 16, 22 dBm.
- **Callback handover (0x4C02):** at boot `lora_ident` (`lora/lora_ident.c:39`, registered at :46) owns the id and takes the
  IDENT answer; `lora_start` registers `on_peer_data` (`lora/lora.c:635`) and takes it over. From then on `lora_ident`
  forwards to `lora_ident_live` (answer received by the LoRa task, `lora.c:719`). A second owner of the id would starve one
  of them.
- **After a C6 reset:** `lora_c6_lost` / `lora_c6_back` (`lora/lora.c:689`, `:708`) stop and restart the polling and
  re-send CONFIG, then `RX_START`.

### 3. C6 boot, flash and IDENT
- **Files** in `/storage/c6` (`C6_IMAGE_DIR`, `c6link/c6_paths.h`): `bootloader.bin`, `partitions.bin`, `firmware.bin`,
  each with a `.md5` (`c6link/c6_firmware.c:214`). A `.md5` that does not describe its `.bin` is refused
  (`c6_md5_matches`). **`ota-c6` order: the `.bin` first, then its `.md5`**, all six files each time; the C6 stays
  untouched if it fails, so repeat before any reboot.
- **Plan** (`c6_boot_plan`): a warm software restart with an unchanged bundle key skips the ROM check; everything else,
  a forced reflash (NVS `c6_force`, `c6link.c:268`), a `c6_verify` flag or a trial image, checks fully. NVS keys:
  `c6_ok_key` (bundle key `md5,md5,md5,size,size,size`, stored only after attach and IDENT MATCH), `c6_verify`
  (`c6link.c:1296`), `c6_rt_restart` (`c6_sup.c:44`), `c6_force`. A skip that loses its bet (no attach, IDENT not
  proven) restarts once with the verify flag, only if that flag persisted (`c6_restart_after_attach`).
- **Bundle changes:** a push into `/storage/c6` over Wi-Fi (`[c6]` file) calls `c6link_bundle_changed`
  (`ota/ota_server.c:405-408`) before touching the file, so the stored key is forgotten and the next boot checks fully.
  Every writer of that directory must do the same.
- **P4 OTA validity needs the C6:** a pending image is marked valid only with the UI up and `c6link_up()` after
  `OTA_VALID_AFTER_S` (`ota/ota_valid.c:3-5`, `main.c` `ota_trial_wait`), and it is rolled back when the link never comes up
  (`ota_valid.c:7-9`). A broken C6 link rolls back every P4 OTA; this is why `ota-c6` goes first.
- **IDENT** (#36): after attach the P4 asks the running C6 app for its ELF sha256 (32 bytes, `LORA_MSG_IDENT`) and
  compares with the bundled `firmware.bin` (`c6_ident_check`; wait `C6_IDENT_WAIT_MS` 2000, `c6link.c:1300`). Only MATCH
  proves the C6; NO_ANSWER, MISMATCH, NO_EXPECTED, NOT_ASKED are all "not proven". After a runtime re-attach IDENT is
  asked again (`c6_sup.c:330`) once LoRa is back. `fw c6` shows the result.
- **Flasher** (`c6link/c6_flasher.c`): UART1 on GPIO15/16, reset GPIO12 (EN), boot strap GPIO13; it releases the C6 into its
  application before ESP-Hosted may talk to it.

### 4. Hardware couplings
- **C6 reset resets the keyboard:** GPIO12 (C6 EN) also resets the TCA8418. The C6 starts before the keyboard
  (`START_C6LINK`, then `START_KEYBOARD`, then `START_LORA` in `firmware/main/boot_order.c`; `START_C6_HOOKS` only after
  all three, pinned by `host_tests/boot_order_test.c`). A later C6 reset runs `keyboard_reinit`, `backlight_reapply`, `lora_c6_back` in that order
  (`main.c:232` `c6_up`).
- **Backlights go through the C6:** UART1 text command `\nBL <display> <keyboard>\n`
  (`board/backlight_level.c` `backlight_cmd_format`, sent from `board/backlight.c`). With the C6 down brightness does not
  change, and Settings says so.
- **R16 / GPIO5 hold (#12, #39):** stock badges have no R16 so GPIO5 is not on `DISPLAY_BL`; with the bridge fitted the P4
  holds the backlight while the C6 is in reset or its ROM (`board/r16_hold.c`; flasher order in `c6_flasher.c:106-120`: EN
  low, GPIO5 back to input, then EN rises). Power off drives backlights off (`main.c:215`), then `c6link_hold_in_reset` (`main.c:216`, defined at `c6link.c:473`). The hold starts once the ROM answered, not before.
- **C6 bootloader and backlight (#39):** there is no C6 bootloader hook (dropped by decision). The P4 rewrites the C6
  bootloader at 0x0 when its MD5 differs from the bundle's (`c6_flasher.c:68`); a broken C6 bootloader means USB-only
  recovery. Until the first `BL` line arrives (`c6/main/backlight_cmd.c`) the C6 backlights run at their boot levels
  (`c6/main/app_main.c`, `backlights_start`), so a P4 that never sends one leaves the panel at the C6 default.
- **UART1 is shared** by the flasher, `fw c6log` and the backlight commands (`board/backlight.c:21`); only one may own it
  at a time. `backlight_start` must come after `c6link_start` (it needs the UART the flasher released).
- **`esp_hosted_init` failure is sticky** until the next boot (`c6_sup.c:278`); only a failed connect is retried by a
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
- Tick: move the P4 from 100 Hz to **1000 Hz** (the C6 already runs 1000). Gives 1 ms sleeps for the mesh loop, the
  keyboard and LVGL, at under 0.5% CPU. Measure idle CPU and free DMA RAM before and after.

## Cores on the P4

The rule: **core 1 draws, core 0 talks to the world.**

| Core | Runs |
|---|---|
| 0, connectivity and services | ESP-Hosted SDIO tasks (prio 22), esp_timer (22), lwIP tcpip (18, pinned), LoRa link task (3), MeshCore and other apps' background parts (3), OTA server (5), console (5), keyboard polling (4), main |
| 1, screen | the LVGL task (pinned; today it floats) and every app's screen code, which runs in LVGL's context |

- The screen never stutters because of signature checks, Wi-Fi traffic or OTA decompression.
- The radio path (C6 → LoRa task → queue → MeshCore) stays on one core.
- The cost: during a large Wi-Fi push MeshCore gets less time; its queue absorbs that, because packets arrive seconds
  apart.
- Today MeshCore is pinned to core 1 and LVGL floats: both change.
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
  the PSRAM check, the clock, an update, the C6 coming back, the SD card, a sensor, low storage).
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

Status: **target design (OS v2, #216)**. Today the tree has three levels in practice (IDF/board/drivers, everything
else, MeshCore) and about 300 call sites that break the layering below. They are listed in the layer baseline
(`tools/layers-baseline.txt`, #218); new violations fail the build, the baseline only shrinks, and it reaches 0 at the
W9 close-out (#237). Until then read this section as where the code is going, and the baseline as how far it is.
The two-chip protocol is a separate contract: see "Two-chip contract" in this file.

| Layer | What | Component(s) |
|---|---|---|
| L0 platform | IDF, FreeRTOS, the LVGL library, `fw_console` | `fw_console`, `psram_guard`, `lichen_random`, `assets`, `lichen_nvs` (#235 revisits it) |
| L1 board/HAL | pins, buses, panel, no NVS | `board`, `storage`, `esp_lcd_st7703`, `esp_tca8418` |
| L2 drivers | LoRa, C6 link, SD, sensors, keyboard chip | `lora`, `c6link`, `sdcard`, `sensors`, `lichen_led`, `lichen_haptics`, `battery`, `lichen_audio` |
| L3 OS services | settings (the only NVS owner), fs, notify, clock, radio, net, power, display/UI (the only LVGL-lock owner), OTA, boot, app registry | `lichen_os` (new, #220); today `lichen_notify`, `lichen_clock`, `lichen_net`, `lichen_boot`, `lichen_mdns`, `ota`, `crash` |
| L4 the gate | headers, capability and identity checks, one ops table; no hardware dependencies | `lichen_api` |
| L5 system | the shell and the system screens, a privileged client of the gate | `lichen_shell` |
| L6 apps | the plain apps, MeshCore | `lichen_apps` (#180), `mesh` |

`main` is the composition root: it may only call init, start and stop of the layers. `layers.conf` lets `main` depend on every layer for now and catches its owner-only symbol uses; tightening that comes with #210.

**Allowed calls (a layer calls itself and the layers below it, with two exceptions):**

| From \ To | L0 | L1 | L2 | L3 | L4 | L5 |
|---|---|---|---|---|---|---|
| L1 board | yes | | | | | |
| L2 drivers | yes | yes | | | | |
| L3 services | yes | yes | yes | | | |
| L4 gate | yes | | | ops table only | | |
| L5 shell | yes | no | no | no | yes (both surfaces) | |
| L6 apps | yes (plain libs) | no | no | no | app surface only | no |

- **Callbacks go up, calls go down.** A lower layer never names a higher one. When it must tell a higher layer
  something (the radio switch, a C6 event, a UI request) the higher layer registers a callback or fills an ops table
  at boot (the Flipper "record" and Zephyr driver pattern). That also removes today's upward link-time edges (the gate
  defining functions that live in the shell, drivers calling the gate).
- **The shell reaches hardware and shared state only through the system surface**, never through drivers or
  `board` directly. `lvgl_port_lock`, `nvs_open`, `heap_caps_*`, `xTaskCreate*` and `lv_group_set_default` become
  owner-only symbols, with the owner listed per symbol.
- **Where things sit:** the shell and the system screens (Settings, About, Files, Power, Update...) are L5; MeshCore and
  the plain apps are L6; the C6 is a second chip behind the c6link driver (L2) and its protocol is the two-chip
  contract, not a layer.

**Enforcement** (`tools/check_layers.sh` with `tools/layers.conf`, #218; it absorbs `tools/check_app_boundary.sh`'s
rules and runs at the end of `build.sh build`): A, `REQUIRES` rules; B, includes; C, symbol ownership through `nm`,
which catches link-time upward edges; D, capabilities (#229, not in the checker yet). Ratchet: the build fails on a
violation that is not in the baseline (NEW, WORSE), on a baseline entry that was fixed and not removed (STALE) and on one
that grew against `main` (GREW), until the baseline is 0 after wave L-C. The app profile keeps the old hard guarantees
that no baseline can excuse: forbidden symbols, libraries and headers outside the closed L6 lists, an unresolved
`${VAR}` in a `REQUIRES` list. Rule C counts per object file, A per edge, B per include line. Apps (`mesh`) still have
named `peer` lines in `layers.conf` for `lichen_notify`, `lichen_clock`, `lichen_random`, `lora`, `fw_console` and
`psram_guard`: they are debts the app surface removes, not part of the target matrix above.

**The gate has two surfaces,** detailed in [app-api.md](app-api.md): the app surface (`lichen_*.h`) for L6, and the
system surface (`lichen_sys_*.h`) for L5, only with the `SYSTEM` capability. There is no syscall trap and no export
table: the P4 runs everything in M-mode with no configurable MPU, and U-mode/TEE is not available on it, so a trap would
protect nothing and cost a context switch.

**Capabilities are an accident and audit guard, not a security boundary.** Apps are compiled into the image and
share its address space, so a determined app can bypass any check. The point is to catch mistakes (an app that quietly
starts transmitting or opens the SD card) and to give review a list of what each app asked for.

**Fault isolation is detection and containment, not protection:** per-app grants (memory, PSRAM quota, booked timers)
freed whole on stop; cooperative stop with a deadline (foreign tasks are never deleted); crash attribution through RTC
no-init and a boot-loop breaker (2 crashes attributed to one app in 3 boots switch its background off and notify);
task-watchdog heartbeats; quotas that refuse rather than exhaust. Waves L-D (capabilities) and L-E (identity,
lifecycle, crash attribution) come after the MVP.

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
