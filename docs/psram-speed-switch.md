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
- **NVS record** `psram_spd` = {want 80/120, trial, failed}; the rules are `psram_speed_logic.c` (host-tested).

## Boot sequence

1. PSRAM init reads and clears STORE15, runs at 80 or 120 MHz, boot tuning + dense boot check as always.
2. `app_main`, right after NVS is up (`psram_speed_boot`, before the one-shot delayline, storage and MeshCore):
   - record `trial` set but running 80: the 120 MHz boot never got through (crash, watchdog, restart before it passed):
     `want = 80`, `failed = 1`, `trial = 0`. Never re-armed by itself.
   - want 120, running 80, not failed: `trial = 1`, arm STORE15, `esp_restart` once (log: "restarting once to boot at 120 MHz").
   - running 120 with the record agreeing: this boot is a trial (`trial = 1`).
3. A live trial (`psram_speed_tick`) passes after 60 s with a clean boot check AND one automatic DMA gate round (all
   patterns, the SD legs only with a card; on the 6 KB `psram_trial` task). It fails at once on a fallback tuning, a bad boot
   check, or a failed gate. A fail: `want = 80`, `failed = 1`, `esp_restart` at 80 (no app save hooks).
   While the running image is still PENDING_VERIFY (OTA not yet marked valid) nothing restarts for the speed: the arm waits
   until the image is valid, a fail only records itself and the tick leaves 120 once valid, so a good OTA is never rolled back.
   A record that cannot be saved arms nothing.
4. While a trial is live MeshCore does not start (its journal is not read or written over a bus on trial). It starts
   by itself when the trial passes (if start at boot is on), or by hand (`fw mesh start`).
5. After a PASSED trial a clean restart at 120 (`esp_restart`: update, Power > Restart, `fw reboot`) keeps 120 MHz through a
   shutdown hook that re-arms STORE15 (RAM flag only, set only after the pass: a restart mid-trial comes back at 80 and
   counts as a failed trial; a factory reset clears it). A panic, WDT or brownout runs no hook, so it comes back at 80. A cold power-on is 80
   first, then one restart into 120.

A failed `fw psram dma` / `fw psram dma sweep` at 120 MHz counts as a failed trial (any bad byte on the AXI-GDMA legs or the
bounced SD leg, or a DMA error; the direct SD leg is reported, not judged). It restarts the badge at 80.

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
   at 80 and `fw psram` shows `failed yes`; it does not try 120 again. `fw psram speed 120 yes` clears the failed flag.
5. Settings > For developers > PSRAM speed: value text, careful dialog "Restarts the badge. If 120 MHz fails, it comes back
   at 80.", choosing 80 restarts at 80.
6. Back to 80 any time: `fw psram speed 80 yes`, or flash/OTA a build without the patch (80 MHz, record ignored).
