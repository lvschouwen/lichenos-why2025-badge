# PSRAM speed switch: 80 / 120 MHz in one image (#121, #353)

The badge runs its PSRAM at 120 MHz by default (#353). It falls back to 80 MHz only on PSRAM evidence seen at 120 and
stays there until the person picks a speed again in Settings > For developers > PSRAM speed or with
`fw psram speed 120 yes` (or `80 yes`). There is no trial: until #353 every 120 MHz boot ran a 60 s trial, and every
fall to 80 it caused was our own doing (#255 a panic on the way into the trial, a `fw reboot` inside it). The one real
120 MHz problem (#120/#121, SDMMC DMA flipping bit 3) is handled by the SD bounce buffer (E4).

## How it works

- **IDF patch** `tools/idf-patches/0003-esp32p4-psram-runtime-80-120.patch` (on top of 0001, same checkout) adds
  `CONFIG_SPIRAM_SPEED_RUNTIME_SELECT`. PSRAM init (`esp_psram_impl_enable`) first reads `LP_SYSTEM_REG_LP_STORE15`: the word
  `0xC4A1 << 16 | 120` selects 120 MHz (MPLL 480 / 4), anything else 80 MHz (MPLL 320 / 4, the stock 80 MHz values). The init
  CLEARS the register right after reading it. The running speed is `g_mspi_psram_speed_mhz`. The 2nd stage bootloader does not
  build `mspi_timing_tuning.c` and does not init PSRAM: unaffected (0 such symbols in `bootloader.elf`). Latency codes and the
  DQS tuning tables are the same for 80 and 120 MHz, so only the MPLL frequency and the clock divider are runtime.
- **Register carrier** (spike, bench-verified on fw-3e9e56b6): STORE15 survives `esp_restart`, `ota-reboot` and an int-WDT
  reset. ROM claims STORE0..11, IDF uses none of 12..15.
- **NVS record** `psram_spd` = {want 80/120, failed, arms, last fall}; the rules are `psram_speed_logic.c`
  (host-tested in `host_tests/psram_speed_test.c`). Written straight to NVS, not through the settings service (it runs
  before the services; #325). Blob v4 (32 bytes): want, failed, arms, 0, version 4, kind, reset reason, ran-at-120,
  wall time (LE32, 0 = clock not set), uptime (LE32), image version (16 bytes, NUL-padded, cut at 15). Any other size or
  version, the trial's 36-byte v3 record of #329 included (no migration: images are not released, and its falls were
  no evidence), is the default record: 120, nothing failed, last fall none.

## What counts as evidence

Only what shows the PSRAM itself returning wrong data, and only while running 120 MHz:

| Kind (`psram.fail_why`) | Where it is seen |
|---|---|
| `boot_check` | the dense 4 MB boot check (`psram_boot.c`) found bad bytes (single-bit words included), judged in `psram_speed_boot` |
| `canary` | a canary block flipped (`psram_guard.c` `verify_one`, every canary within ~0.7 s) |
| `heap` | the PSRAM heap failed `heap_caps_check_integrity` (CORRUPT HEAP), checked once a minute by the guard task at 120 MHz |
| `dma_gate` | a DMA gate round failed: `fw psram dma`, `fw psram dma sweep`, the Bench's round, the stress run's rounds |

Not evidence: any reset reason (a restart, an OTA reboot, a panic, a watchdog, a brownout, a power cycle), and a
fallen-back or short tuning window without bad bytes (it still marks the boot bad and raises "PSRAM check failed at
boot"). A CORRUPT HEAP that the heap itself finds first (in a `free`) aborts before anything can be recorded: that
panic comes back at 80 for one session (below) without a fall.

Evidence seen on any task is only reported (`psram_speed_report`, an atomic, the first one of a boot wins); the guard
task's tick judges it once a second and the `psram_speed` job task saves the record and restarts. At 80 MHz evidence
changes nothing (there is nothing to fall to); it is logged where it was found, as before.

## Boot sequence

1. PSRAM init reads and clears STORE15, runs at 80 or 120 MHz, boot tuning + dense boot check as always.
2. `app_main`, right after NVS is up (`psram_speed_boot`, before the one-shot delayline, storage and MeshCore):
   - running 120, the record agrees (want 120, not failed), no bad bytes: nothing written. The shutdown hook keeps 120
     through every clean restart.
   - running 120 with bad bytes in the boot check: the fall (below).
   - running 120 against the record (want 80 or failed): back to 80, no note.
   - running 80 with 120 wanted and not failed:
     - after a panic or watchdog: this session stays at 80, the next clean restart arms 120 (the shutdown hook). No
       fall, and no arm restart either, so a crash at 120 cannot loop through one.
     - otherwise (a power-on, the first boot, a restart that ran no hook): `arms + 1` saved, STORE15 armed, `esp_restart`
       once ("restarting once to boot at 120 MHz"). The third arm in a row whose 120 MHz boot never settled
       (`PSRAM_SPEED_SETTLE_SECONDS`, 60 s of uptime, clears `arms` with one NVS write) keeps this session at 80 and
       starts the count over: it bounds a loop (a brownout at 120, a restart before the hook is in), it is no failure.
3. **The fall:** `want = 80`, `failed = 1`, the last fall recorded, STORE15 cleared, `esp_restart` (no app save hooks with
   a PSRAM that is suspect). Logged at E (#113: a feature lost until someone acts) on the boot that falls and again on
   every boot after it, and raised once per boot as the notice "PSRAM back at 80 MHz" (error level). Never re-armed by
   itself.
4. While the running image is still PENDING_VERIFY (OTA not yet marked valid) nothing restarts for the speed: the arm,
   the leave and the fall's restart wait until the image is valid (the record is already saved), so a good OTA is
   never rolled back. A record that cannot be saved arms nothing.
5. A clean restart at 120 (`esp_restart` and everything on it: update, Power > Restart, `fw reboot`) keeps 120 MHz through a
   shutdown hook that re-arms STORE15. A panic, WDT or brownout runs no hook, so it comes back at 80 (step 2). A cold
   power-on is 80 first, then one restart into 120. A factory reset clears the register and the record: 80, then the
   default record arms 120.

MeshCore starts at boot as at any speed (the trial's hold is gone), and the OTA validity mark no longer waits for a
trial.

## Last fall (#329)

Every fall records: the kind, `esp_reset_reason()` of the boot that fell, ran at 120 (always), the UTC wall time if
the clock was set (else that boot's uptime), and the image version. The E line carries the same text, `fw psram`
shows it:

    psram speed: running 80 MHz; 120 MHz failed, back at 80; want 80, failed yes, arms 0 of 3; IDF runtime-selectable; ...
    psram speed: last fall to 80: canary flip, reset sw, ran at 120, 2026-11-11 14:03:05 UTC, fw-605935b1

RO registry keys: `psram.fail_why` (ENUM none / boot_check / dma_gate / canary / heap; 3..9 are the trial's retired
kinds, kept so the numbers do not move), `psram.fail_rst` (STR), `psram.fail_at120` (BOOL), `psram.fail_fw` (STR),
`psram.aborts` (U8, the arm count since #353), `psram.want` and `psram.clk` (what runs). Choosing a speed clears
`failed` and `arms`, not the last fall.

## Journal safety

E4 (#121) bounces every SD transfer with a PSRAM buffer through internal RAM. Not audited here: any other DMA path that
moves journal data into or out of PSRAM (none known: the display reads PSRAM, SDIO to the C6 uses internal buffers).

## Build

The badge's IDF carries `tools/idf-patches/0001` and `0003` (`git -C ~/esp/esp-idf-v6.1 apply` each, 0001 first), and
`firmware/sdkconfig.defaults` sets `CONFIG_SPIRAM_SPEED_RUNTIME_SELECT=y`, so the normal build
(`BADGE_IDF_PATH=~/esp/esp-idf-v6.1 ./firmware/build.sh build`) is the switchable image. An IDF without 0003 still builds:
the symbol is unknown to Kconfig, the app reports 80 MHz and "not supported", and the Settings row is hidden.

## Bench plan

1. OTA the image. If the badge was at 80 by an old record (v3), the first boot reads the default and restarts once into
   120: `fw psram speed` shows `running 120 MHz; 120 MHz; want 120, failed no, arms 1 of 3` and, after 60 s,
   `arms 0 of 3`. `fw get psram.clk` is 120 MHz.
2. `fw reboot` within 20 s of a boot: back at 120, `failed no`, no "120 MHz failed" line in the log.
3. `fw crash test wdt` (or any crash): the next boot is at 80 with "this boot stays at 80 MHz (after a crash, no evidence
   against 120)"; `fw reboot`: back at 120, `failed no`.
4. Gate at 120: `fw psram dma sweep 16384` x3, `fw psram dma 15` x7, `fw mesh hist scan 100`. A failed gate restarts the
   badge at 80 with "120 MHz failed, back at 80" in Settings, the notice "PSRAM back at 80 MHz", and `last fall to 80:
   DMA gate, ...`.
5. `fw psram speed 120 yes` after a fall: one restart, straight to 120, `failed no`.
6. Settings > For developers > PSRAM speed: value text, dialog, choosing 80 restarts at 80 and stays there (no arm).
