# Lichen architecture: what runs where

Agreed with Lucas on 2026-10-30. Companion to [shell-design.md](shell-design.md) (the screens). Lichen is the OS on
the WHY2025 badge (lean firmware, #61); MeshCore is its first app (#67).

## Two chips

**P4 (Lichen):** dual-core RISC-V at 360 MHz (rev v1.0 silicon), 32 MB PSRAM at 80 MHz (at 200 MHz this board flips
bits in PSRAM words, #89/#93; `fw psram` watches it with canaries), 16 MB flash.
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
 keyboard TCA8418 ─I2C────┤ ◄══════════════════════════════════►│   peer data = LoRa protocol v3
 BMI270 / BME690 ─I2C─────┤   UART: flashing the C6 at boot,    ├─ SPI ─ SX1262 (RA-01SH) + bodges
 SD card ─────────────────┤         backlight commands          ├─ PWM: screen + keyboard backlight
 flash: Lichen ×2, /storage│   GPIO12 → C6 EN (resets TCA8418 too)
                          │   GPIO13 → C6 boot strap, GPIO14 ← C6 INT
```

Links between the chips:
- **SDIO (4-bit):** ESP-Hosted carries Wi-Fi, BLE HCI, RPC and peer data. Our LoRa protocol v3 runs on peer data
  (IDs 0x4C01 P4→C6, 0x4C02 C6→P4, `shared/lora/lora_proto.h`). Both chips must run the same protocol version.
- **UART:** the P4 flashes the C6 at boot from `/storage/c6` when the image's MD5 differs; afterwards the same UART
  carries the backlight commands.
- **GPIO:** P4 GPIO12 → C6 EN, which also resets the TCA8418. GPIO13 → C6 boot strap; esp_hosted's `RESET_GPIO=13` is
  wrong, #15. GPIO14 ← C6 interrupt.
- **GPIO0/GPIO1:** XTAL_32K, the Y1 crystal for the RTC slow clock (#63). Reserved: nothing else may claim them.

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
  part.
- **No downloadable apps for now.** Loadable native apps (ELF) were BadgeVMS's biggest source of trouble (per-app
  heaps and stacks, wrapped functions, an export table). A sandboxed script runtime (MicroPython, Lua or WebAssembly)
  is the safe way to open up later and could be one app running scripts through the same interface.
- Others can already write an app as a component and build their own Lichen.

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

## Flashing on the bench

- P4 firmware over USB without touching `/storage` (MeshCore identity, contacts, channels):
  `BADGE_EXTRA_PORTS=/dev/ttyUSB1 tools/badge.sh flash-fw /dev/ttyUSB1 --no-storage`.
- The C6 image is a file in `/storage/c6`. A full USB flash replaces `/storage` and loses the identity, so C6 updates
  go as two files over Wi-Fi (`firmware.bin`, then its `.md5`) before the P4 flash.
