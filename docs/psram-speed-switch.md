# PSRAM speed switch: 80 / 120 MHz in one image (#121)

The badge ships at 80 MHz PSRAM. 120 MHz can be tried from Settings > For developers > PSRAM speed or with
`fw psram speed 120 yes`; it falls back to 80 by itself when it does not hold.

## How it works

- **IDF patch** `tools/idf-patches/0003-esp32p4-psram-runtime-80-120.patch` (on top of 0001, same checkout) adds
  `CONFIG_SPIRAM_SPEED_RUNTIME_SELECT`. PSRAM init (`esp_psram_impl_enable`) first reads `LP_SYSTEM_REG_LP_STORE15`: the word
  `0xC4A1 << 16 | 120` selects 120 MHz (MPLL 480 / 4), anything else 80 MHz (MPLL 320 / 4, the stock 80 MHz values). The init
  CLEARS the register right after reading it. The running speed is `g_mspi_psram_speed_mhz`. The 2nd stage bootloader does not
  build `mspi_timing_tuning.c` and does not init PSRAM: unaffected (0 such symbols in `bootloader.elf`). Latency codes and the
  DQS tuning tables are the same for 80 and 120 MHz, so only the MPLL frequency and the clock divider are runtime.
- **Register carrier** (spike, bench-verified on fw-3e9e56b6): STORE15 survives `esp_restart`, `ota-reboot` and an int-WDT
  reset. ROM claims STORE0..11, IDF uses none of 12..15.
- **REACHED marker** (#329): as soon as PSRAM init runs at 120 MHz (the tuning wrappers in `psram_boot.c`, again in
  `psram_speed_boot`) the boot writes `0xC4A2 << 16 | 120` to STORE15. PSRAM init takes any word but the ARM word as
  80 MHz and keeps the raw word (`psram_speed_carrier_raw`), so the next boot runs at 80 and knows the boot before it did
  run at 120 and ended without a pass, a fail or a clean re-arm. Cleared on pass and fail; the shutdown hook's re-arm
  overwrites it. Nothing is written to flash at 120 MHz before the trial passes.
- **NVS record** `psram_spd` = {want 80/120, trial (0 / armed), failed, aborts, last failure}; the rules are
  `psram_speed_logic.c` (host-tested). Written straight to NVS, not through the settings service (it runs before the
  services; #325). Blob v2 (32 bytes, #329): want, trial, failed, aborts, version 2, kind, reset reason, ran-at-120,
  wall time (LE32, 0 = clock not set), uptime (LE32), image version (16 bytes, NUL-padded, cut at 15). Any other size or
  version, the 4-byte record of images before #329 included (no migration: images are not released), is the default
  record (80, nothing pending, last failure none).

## Boot sequence

1. PSRAM init reads and clears STORE15, runs at 80 or 120 MHz, boot tuning + dense boot check as always.
2. `app_main`, right after NVS is up (`psram_speed_boot`, before the one-shot delayline, storage and MeshCore):
   - running 80 with a trial pending (record armed, or REACHED found with 120 wanted and not failed):
     - REACHED found: that 120 MHz boot did not get through: `want = 80`, `failed = 1`, `trial = 0`, last failure
       "crash or watchdog" (reset reason panic / int_wdt / task_wdt / wdt / cpu_lockup) or "restart before it passed"
       (anything else), ran at 120. Never re-armed by itself.
     - no marker, a panic / watchdog reset: failed too, "crash or watchdog while armed (no 120 MHz marker)". Fail-safe:
       until #330 shows STORE15 survives every crash reset, a missing marker after a crash proves nothing.
     - no marker, power-on / ext / brownout: still armed, not counted, no note (a power cycle is no evidence).
     - no marker, any other reset: the switch never took effect (#329: the restart path once panicked at 80 and that
       counted as "120 MHz failed"). Last failure "aborted (ran at 80)", `aborts + 1`, the trial stays armed and the
       shutdown hook arms STORE15 on the next clean restart (no restart of its own). The third abort in a row gives up:
       failed, "aborted 3 times, given up".
   - want 120, running 80, not failed: `trial = 1`, arm STORE15, `esp_restart` once (log: "restarting once to boot at 120 MHz").
   - running 120 with the record agreeing: this boot is a trial (in RAM; the record is not written, the marker is the
     evidence). Running 120 against the record (want 80 or failed): last failure "ran at 120 against the record", back
     to 80.
3. A live trial (`psram_speed_tick`) passes after 60 s (record saved with `trial = 0`, `aborts = 0`, then the marker
   cleared; a pass that cannot be saved keeps the marker) with a clean boot check AND one automatic DMA gate round (all
   patterns, the SD legs only with a card; on the 6 KB `psram_trial` task). It fails at once on a fallback tuning, a bad boot
   check, or a failed gate. A fail: `want = 80`, `failed = 1`, last failure "boot check" or "DMA gate", `esp_restart` at
   80 (no app save hooks).
   While the running image is still PENDING_VERIFY (OTA not yet marked valid) nothing restarts for the speed: the arm waits
   until the image is valid, a fail only records itself and the tick leaves 120 once valid, so a good OTA is never rolled back.
   A record that cannot be saved arms nothing.
4. While a trial is live MeshCore does not start (its journal is not read or written over a bus on trial). It starts
   by itself when the trial passes (if start at boot is on), or by hand (`fw mesh start`).
5. After a PASSED trial a clean restart at 120 (`esp_restart`: update, Power > Restart, `fw reboot`) keeps 120 MHz through a
   shutdown hook that re-arms STORE15 (RAM flag only, set after the pass or with an aborted trial still armed: a restart mid-trial comes back at 80 and
   counts as a failed trial; a factory reset clears it). A panic, WDT or brownout runs no hook, so it comes back at 80. A cold power-on is 80
   first, then one restart into 120.

A failed `fw psram dma` / `fw psram dma sweep` at 120 MHz counts as a failed trial (any bad byte on the AXI-GDMA legs or the
bounced SD leg, or a DMA error; the direct SD leg is reported, not judged). It restarts the badge at 80.

## Last failure (#329)

Every judgement (failed or aborted) records: the kind, `esp_reset_reason()` of the boot that judged it, whether the
failed boot ran at 120 MHz, the UTC wall time if the clock was set (else the judging boot's uptime), and the image
version. The judging `ESP_LOGE` line carries the same text, `fw psram` shows it:

    psram speed: last failure: crash or watchdog, reset task_wdt, ran at 120, 2026-11-11 14:03:05 UTC, fw-605935b1
    psram speed: last failure: none

(with `; aborted trials in a row N of 3` while aborts are counted). RO registry keys: `psram.fail_why` (ENUM none /
boot_check / dma_gate / crash / restart / aborted / gave_up / crash_armed / against_rec), `psram.fail_rst` (STR), `psram.fail_at120` (BOOL),
`psram.fail_fw` (STR), `psram.aborts` (U8). The record stays until the next failure; choosing a speed clears `failed`
and `aborts`, not the last failure. For a crash or restart the uptime is the judging boot's (seconds), not how long
the failed 120 MHz boot ran.

## Journal safety

MeshCore waits during a trial (above). E4 (#121) bounces every SD transfer with a PSRAM buffer through internal RAM. Not
audited here: any other DMA path that moves journal data into or out of PSRAM (none known: the display reads PSRAM, SDIO
to the C6 uses internal buffers).

## Build

The patched IDF is a copy: `~/esp/esp-idf-v6.1-p0003` (main's IDF with 0001, plus 0003). To adopt 0003 in the main IDF:
`git -C ~/esp/esp-idf-v6.1 apply tools/idf-patches/0003-esp32p4-psram-runtime-80-120.patch` (0001 must already be applied).

Main's normal build (`BADGE_IDF_PATH=~/esp/esp-idf-v6.1 ./firmware/build.sh build`) still compiles without 0003: the symbol
is unknown to Kconfig there, the app reports 80 MHz, "not supported", and the Settings row is hidden. Patched build,
own build directory and sdkconfig, run after `build.sh` has built the C6 image and staged the storage:

    . ~/esp/esp-idf-v6.1/export.sh   # tools; the p0003 copy's own export.sh finds no tools
    export IDF_PATH=$HOME/esp/esp-idf-v6.1-p0003
    python $IDF_PATH/tools/idf.py -C firmware -B firmware/build-p0003 -DSDKCONFIG=$PWD/firmware/build-p0003/sdkconfig build

Image: `firmware/build-p0003/badge_fw.bin`.

## Bench plan (one OTA, MeshCore does not start during the trial)

1. OTA the image. Boot at 80: `fw psram` shows `psram speed: running 80 MHz; 80 MHz; want 80 ... IDF runtime-selectable`,
   carrier found `00000000`. Behaviour identical to before.
2. `fw psram speed 120` (prints what it does), then `fw psram speed 120 yes`: the badge restarts once (it arms and restarts), the boot log shows `PSRAM Speed: 120MHz`. `fw psram`: running 120, trial running; MeshCore
   not started. After 60 s: log "120 MHz trial passed", MeshCore starts.
3. Gate at 120: `fw psram dma sweep 16384` x3, `fw psram dma 15` x7, `fw mesh hist scan 100`. A failed DMA gate restarts the
   badge at 80 with "120 MHz failed, back at 80" in Settings.
4. Forced failure test: with 120 running and the trial live (first 60 s), `fw crash test wdt` (or any crash): the next boot is
   at 80 and `fw psram` shows `failed yes` and `last failure: crash or watchdog, reset task_wdt, ran at 120, ...`; it does
   not try 120 again. `fw psram speed 120 yes` clears the failed flag.
5. Settings > For developers > PSRAM speed: value text, careful dialog "Restarts the badge. If 120 MHz fails, it comes back
   at 80.", choosing 80 restarts at 80.
6. Back to 80 any time: `fw psram speed 80 yes`, or flash/OTA a build without the patch (80 MHz, record ignored).
