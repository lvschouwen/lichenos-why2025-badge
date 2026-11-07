# OS v2 MVP bench checklist

The one bench pass for the OS v2 MVP candidate (#216, close-out #237). It collects the "waits for the v2 MVP flash"
items of the wave issues (#180, #205, #209, #210, #220-#228, #234-#236, #244, #245) into one order, deduplicated. Format
as in CLAUDE.md "Bench check": action -> **Expected:** text. Strings are quoted from the code at `ee439459` plus this
commit; `<...>` is a value that differs per badge. Procedure and rules: the `lichen-bench` skill (main session only).

Rules for the whole pass:
- Wait 75 s after every reboot before a console command. Console lines are `fw <command>`.
- On air only `#badgetest` (or the named test DM target). No send to anyone else, no radio setting changes.
- **Bold** = destructive or hard to undo. Only Lucas decides the fallback flash with `--wipe-storage`.
- No IPs, MACs, host names, SSIDs or tokens in issue comments; screenshots stay out of the repo.

## Bench check (fw-<build>)

**What changed:** the whole OS v2 restructure W1-W8b and the close-out (layers L0-L6 with a layer baseline of 0, the
gate and its system surface `lichen_sys_*.h`, the UI service with the lock-owner check, the task and memory books, one
NVS owner, c6link and lora behind hooks, notify pushing to the LED and haptics, the boot order table, the new boot logo
text). Nothing should look or behave differently except the "Lichen OS 2" name and the new console replies listed here.

**Setup:** the main checkout at the commit under test, `./firmware/build.sh reconfigure build` done so the image reads
`fw-<sha>`; the C6 bundle in `firmware/storage_staging/c6` current; the badge on USB; a second badge or Heltec on
`#badgetest`; Wi-Fi in reach. Record the three baselines below before the flash.

### 1. Pre-flash: snapshot the identity and the baselines (old image still running)

- [ ] `fw mesh` -> note `mesh id <4 bytes hex> "<name>" | rx .. | tx .. | N contacts, M messages` and
  **Expected:** `identity: ok`.
- [ ] `fw wifi` -> **Expected:** `wifi: "<ssid>" connected, ip <ip> (lichen-xxxx.local), rssi ..`. Note the SSID.
- [ ] `fw mem` -> note the `mem dma free .. min ..` line (the pre-MVP baseline was free 99979 / min 89611).
- [ ] `fw bl` -> note `backlight <display> <keyboard>`; `fw clock` -> note the source.
- [ ] `tools/badge.sh backup <port>` -> a full flash image in the backups directory (it holds `/storage` and NVS).
  Check the `.sha256` next to it. **This is the only copy of the identity.** Read-only from here on: never write to it.
- [ ] Optional, for the fallback in step 3: list the backup's FAT storage slice read-only and keep these files beside
  the backup: `/meshcore/identity/_main.id` (96 B), `/meshcore/contacts3`, `/meshcore/channels2`, `/meshcore/prefs.json`,
  `/lora.toml`, `/lichen/notify.bin`. `/ota_token.txt` and `/c6/*` are not restored (below).

### 2. Flash (Lucas only)

Recommended: **without** `--wipe-storage` (`badge.sh flash-fw`'s default). The whole-tree review found that v2 changed no
storage format and no NVS key, so `/storage` and NVS carry over and nothing needs restoring.

- [ ] `BADGE_EXTRA_PORTS=<port> tools/badge.sh flash-fw <port>` -> writes bootloader, partitions, app and assets and
  keeps the storage partition. Power-cycle and wait 75 s.
- [ ] Boot screen -> **Expected:** the mark with `Lichen OS 2` in large text under it and the fw version at the bottom
  (#236). Any key during boot shows the details view with the mark, `Lichen OS 2` and the version.

### 3. Restore (only after the fallback flash) and Wi-Fi

- [ ] Recommended path: nothing to restore. Wi-Fi comes back from NVS: `fw wifi` -> `wifi: "<ssid>" connected, ip ...`,
  **Expected:** no `Wi-Fi driver restore` warning on this first boot (#235). Settings > Wi-Fi lists the network under
  `Saved networks`. Go to step 4.
- [ ] **Fallback (Lucas decides): `tools/badge.sh flash-fw <port> --wipe-storage`** replaces the whole storage
  partition from `storage_staging`; the C6 images in `/c6` come back from the build, the rest is gone. MeshCore then
  makes a new identity (`fw mesh` -> `mesh id` differs from step 1). NVS (settings, Wi-Fi) stays, so Wi-Fi rejoins; if
  not, Settings > Wi-Fi > scan, pick the network, enter the password.
- [ ] **Fallback: restore the files from the backup slice** (replaces the new identity), each with its exact path
  (`ota_path.c`: `FLASH0:[a.b]name` is `/storage/a/b/name`, `FLASH0:name` is `/storage/name`):
  `tools/badge.sh ota-file <host> <file> <target>` for
  `FLASH0:[meshcore.identity]_main.id`, `FLASH0:[meshcore]contacts3`, `FLASH0:[meshcore]channels2`,
  `FLASH0:[meshcore]prefs.json`, `FLASH0:lora.toml` and `FLASH0:[lichen]notify.bin`. Do not push `/ota_token.txt`
  (the OTA token is the new image's own; use the new one for `badge.sh`) or `/c6/*`. Then `tools/badge.sh ota-reboot
  <host>` and wait 75 s.

### 4. Identity check

- [ ] `fw mesh` -> **Expected:** `mesh id` shows the same 4-byte prefix and name as the pre-flash snapshot,
  `identity: ok`, and the contacts and messages counts match.
- [ ] `fw mesh` does not say `identity: MISMATCH, TX blocked` or `identity: file unreadable, TX blocked` (those block
  all TX; if seen, stop and report).
- [ ] One message to `#badgetest` goes out and is heard by the other badge (identity accepted on air).

### 5. The #234 two-chip gate

Contract: `docs/architecture.md` "Two-chip contract". Any failure stops the pass.

- [ ] Cold boot reaches home; `fw boot` shows the C6 step done with `N ms`, then Wi-Fi and clock. `fw c6` -> **Expected:**
  `c6: this boot ..., IDENT match (N ms)`.
- [ ] `fw wifi` -> `wifi: "<ssid>" connected, ip ...`, then `fw wifi ps` -> **Expected:** `wifi: power save off`.
  `tools/badge.sh ota-info <host>` answers with the firmware version.
- [ ] `fw mesh` shows `rx N` rising between two reads; one TX to `#badgetest`.
- [ ] `fw c6` -> note `restarts`; `fw keys on`; `fw c6 reset`; wait 75 s. **Expected** in the log: `wifi: down for the C6
  re-attach ...`, then `wifi: started after the C6 re-attach`, then `stack free: c6sup N, c6wifi M` with M > 0. One
  physical key press logs a key (`fw keys send` bypasses the keyboard chip and proves nothing), LoRa `rx N` counts again,
  `ota-info` answers, `fw c6` reads `up` with `restarts` one higher.
- [ ] `fw bl 40 40` -> **Expected:** `backlight 40 40` and the panel follows (this path runs through the C6). Restore
  the noted levels.
- [ ] `fw mem` -> **Expected:** `mem dma free N ... min M` with **N >= 89981 and M >= 80650** (the pre-MVP 99979 / 89611
  minus 10 %). Also `mem internal ... free_pct .. min_pct ..` is noted (the #205 totals).
- [ ] `fw airplane` -> **Expected:** `airplane off | wifi on | lora on | bluetooth off`.
- [ ] `fw tasks` -> no task without an owner except the IDF, esp_hosted and LVGL tasks.

### 6. Per-issue items (grouped by area, in walk order)

Boot and the OS name (#210, #236, #244):
- [ ] `fw boot` shows the same rows as before, all done; the serial boot log matches the previous build line for line and
  has no `has no start function` line.
- [ ] The boot screen shows the step bar and failed-step lines; Home appears about 2 s after the apps step.
- [ ] `fw boot fail clock`, reboot -> **Expected:** the clock step shows `simulated (fw boot fail)` once; the next boot is
  normal.
- [ ] Soft restart (Settings > Power > Restart, or `ota-reboot`) shows `Restarting...` for about 2 s, then the same boot
  screen as a cold boot.
- [ ] About -> the heading reads `Lichen OS 2`, the version tile `Lichen OS 2 (fw-<sha>)`; `fw ui overflow` -> **Expected:**
  0 overflowing labels.
- [ ] `fw ui perf reset`, a few physical keys, `fw ui perf` -> a line `ui perf latency: render p95 N ms, key-to-flush K keys
  avg .. max .. p95 .. ms, first frame at N ms` with `first frame at` under 3000 ms (#205; fresh boot).

Apps and navigation (#180, #221, #225):
- [ ] Clock, Timer, Calculator, Weather, Notepad, QR code, Keyboard test and LoRa spectrum each open from home and work as
  before; Esc goes back from each; Notepad lists notes and to-dos.
- [ ] Default home order with no saved order: MeshCore, Settings, About, Files, the other system apps, then the 8 plain
  apps.
- [ ] Opening a MeshCore chat twice reuses the screen. Keyboard test: the raw keys draw the layout and hold-Esc leaves.
  The Clock dial and date line show.
- [ ] `fw lock now` locks, `fw lock unlock` lifts it. The console set `fw ui`, `fw ui overflow`, `fw keys send`, `fw ui open`,
  `fw whatsnew`, `fw flashlight`, `fw screensaver test`, `fw uicheck`, `fw ui perf bench 10` and a screenshot give the
  same replies as before.
- [ ] Timer: a short countdown, go home -> **Expected:** the notice `Timer finished` arrives.
- [ ] `fw sensors` stop and start, and Weather shows readings like before. Vibration on/off and strength (a tick, kept
  after a reboot), `fw vib`.
- [ ] Shape keys open Home, Messages, Switch app, Search, Notifications, Power. The remote key lock and the screen-saver
  wake key work.

Fonts, tasks and memory (#220, #228):
- [ ] `fw fonts` -> **Expected:** three lines `fonts <px> px: custom, <bytes> bytes PSRAM, line <n>`.
- [ ] `fw tasks` lists `sys_notices`, `thermal` and `factory_keys` with owner `lichen`; `filesjob`, `wifian` and `wifiscan`
  appear with owner `lichen` while they run and are gone afterwards. No `booked but not in the task list` line.
- [ ] `fw mem` shows a `mem app lichen ...` PSRAM row and a `mem app meshcore ...` row that includes the chat rings after
  a message. About and Benchmark memory lines look as before.

Display, power, thermal (#224, #226):
- [ ] `fw bl 60 20`, reboot, wait 75 s, `fw bl` -> **Expected:** `backlight 60 20`. Settings > Display sliders follow and
  survive a reboot; the screen never goes below 5. Brightness survives `fw c6 reset`.
- [ ] After the wipe flash `fw bl` shows the saved level (NVS was kept; a truly fresh install shows `backlight 30 10`).
- [ ] With the C6 down (during `fw c6 reset`) Settings > Display says `Brightness can't change right now: the radio chip is
  not answering.`
- [ ] Idle: dim, then off, a key brings it back; the log shows `backlight dim`, `backlight off`, `backlight bright`. A
  message pulse and the flashlight both return to the set levels.
- [ ] `fw thermal test warn` -> the minimum brightness, Wi-Fi and LoRa off, HOT in the status bar; `fw thermal test
  clear` restores everything.
- [ ] **Settings > Power > Power off** -> goodbye screen, lights off, C6 held, deep sleep. Wake with the power key and
  wait 75 s.
- [ ] `fw sensors boot off`, reboot, wait 75 s, `fw sensors boot` -> **Expected:** `sensors: start at boot off`;
  `fw sensors boot on` -> `sensors: start at boot on (saved)`.

Wi-Fi, clock, keyboard hooks (#223, #235, #244):
- [ ] `fw wifi off` then `fw wifi on` -> **Expected:** `wifi: radio off`, `wifi: radio on`, it reconnects, and the
  switch survives a reboot.
- [ ] A second saved network survives a reboot; Forget removes the other one for good. The settings backup export (Backup)
  has no `wifi_` lines.
- [ ] Wi-Fi analyser -> **Expected:** rows like `-60 dBm, 3 of 4 bars, channel 6`. the Settings row `Name on the network`
  shows `lichen-xxxx.local`.
- [ ] Settings > Time: a gap hint; set the time back by more than 5 min -> **Expected:** `earlier than before: mesh nodes
  ignore our adverts until then`; a refused set shows `not set: ...`.
- [ ] The status bar clock shows `~HH:MM` when only an app set the time; About shows the clock source. After a warm reset
  `fw clock` -> **Expected:** `carried over the restart, was SNTP` (or the source it had).
- [ ] The factory combo arms (hold Esc+Bksp 3 s at boot, within 10 s of the keyboard coming up) -> `Factory reset:
  settings`. **Cancel it.**

LoRa and MeshCore (#222, #228):
- [ ] `fw lora` shows no `radio switched off`. Settings LoRa off, then a send to `#badgetest` -> refused; the log shows
  `LoRa radio switched off`. LoRa on -> log `LoRa radio switched on: sending the radio config`, then one TX to
  `#badgetest`.
- [ ] `fw lora off` / `fw lora on` -> **Expected:** `lora radio off` / `lora radio on`, and the Settings row follows.
  `fw lora off`, reboot, wait 75 s -> it stays off; then `fw lora on`.
- [ ] MeshCore DM and channel traffic to the test target only, plus a prefs save (a name change and back). With PSRAM
  nearly full a small prefs save still works.

Storage, updates, backup, factory reset (#227, #209):
- [ ] Settings > Updates over Wi-Fi shows `On`, `Off` or `Off (timer)`; switching it off asks first. `Close updates
  after` cycles Never / 10 min / 30 min / 1 h / 2 h. The token still works for `badge.sh ota-*`; airplane mode closes the
  server.
- [ ] Settings > SD card: mount, unmount and format work; the note reads `Apps' files: /sd/lichen`. About shows storage
  free and the SD line (e.g. `28.1 GB free of 29.7 GB`, or `no card`).
- [ ] Files: copy to SD. Settings > Backup: save, then load unchanged -> **Expected:** `The backup matches the settings
  on the badge. Nothing to load.` Change one setting, load -> a 1-change summary, then `Loaded 1 setting. Restart the
  badge to apply all of them.` A backup file over 8 KB -> `Not loaded: ... Nothing was changed.` Declining at the confirm
  leaves the `mem app lichen` PSRAM row back at its old size.
- [ ] Power > Restart with the SD mounted unmounts first. The Crash log opens; after a crash boot a crash notice shows.
- [ ] Settings > Factory reset > Settings -> **Expected:** `Reset all settings and restart? MeshCore and files stay.`
  **Cancel it.**

Notifications, LED, haptics (#245):
- [ ] `fw notify` and `fw led` behave as before. An unread notice -> the blue LED pulses every 4 s; opening Notifications
  marks them read and the LED goes dark.
- [ ] Do-not-disturb on with unread waiting -> the LED stays dark; off -> the pulse comes back. Settings > Notifications >
  Status LED toggle works.
- [ ] A toast with feedback -> one buzz per toast, none during an OTA receive. Reboot with unread history -> the LED
  pulses after boot without a new notice.

Bug-fix issues merged after the review (main fills in each list after merging; the wave must not be called passed
while a placeholder is empty):
- [ ] #248 (lichen_os UI service): TODO bench steps.
- [ ] #249 (lichen_net Wi-Fi, wifi_screen.c): TODO bench steps.
- [ ] #250 (board backlight, c6link flasher; also the two-chip contract's UART1 bullet): TODO bench steps.
- [ ] #251 (board keyboard, factory combo): TODO bench steps.

### 7. Lock-owner check

- [ ] After the walk above: `fw ui lockcheck` -> **Expected:** `ui lockcheck: count mode, armed, 0 hit(s) without the
  LVGL lock`. Any hit lists `<n>x <what> from 0x<addr> on <task>`: report the line and stop.

### 8. UI latency

- [ ] `fw ui perf reset`, 2 min of normal use (home, a few apps, scrolling), `fw ui perf` -> **Expected:** render p95
  at most 33 ms and close to the pre-MVP numbers (above 33 ms is the replan trigger of #216).

### 9. OTA-reboot loop

- [ ] **Ten or more times:** `tools/badge.sh ota-reboot <host>`, wait 75 s, `fw boot` all done, `fw c6` -> `IDENT match`,
  `ota-info` answers. Not one boot loop, rollback or missing Wi-Fi.
- [ ] After an OTA of the same image plus `OTA_VALID_AFTER_S`: the log shows `this image is now valid`.

### 10. Soak (at least 1 h)

- [ ] `fw ui perf reset` and `fw psram` (canaries) at the start; leave the badge on Wi-Fi with MeshCore running, the
  screen on part of the time, a few messages to `#badgetest`.
- [ ] After 1 h or more: `fw ui lockcheck` -> **Expected:** `0 hit(s) without the LVGL lock`.
- [ ] `fw psram` -> **Expected:** `psram no damage seen` and `... 0 hits in the canaries now held`.
- [ ] `fw mem` DMA free and min still at or above 89981 / 80650; `fw tasks` and `fw mem` show no growing app row; `fw ui
  perf` p95 still at most 33 ms; `fw c6` restarts unchanged.

**Pass** -> post the result on #216 and #237, remove `needs-lucas`/`needs-proofing` from the wave issues that passed,
close them, and keep #234 open until the gate has also run after the next wave. A failure becomes a linked priority
issue before anything else is merged.
