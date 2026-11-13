# Bench script: wave C on fw-f742e4ce

Wave C = #406 (host test only), #410 `fw mem window`, #408 transmit air log, #405 per-row remote keys on the Advert & contacts page; plus the open SD-format proof of #368 (wave A). Goal: a brief proof on real hardware. Remote only. File prefix for your output: `wc-`.

## Tools (use exactly these)
- `S=<scratchpad>`
- Console line over Wi-Fi: `"$S/fw.sh" "<line>" ["<line>" ...]` (lines without the `fw ` prefix; it prints `>> line` and the redacted reply).
- Other badge.sh commands need the address: `IP=$(cat "$S/badge-ip")`, then from <repo>: `tools/badge.sh ota-info "$IP"`, `tools/badge.sh log "$IP" > "$S/wc-log-N.txt"`, `tools/badge.sh screenshot "$IP" "$S/wc-N.png"`, `tools/badge.sh ota-reboot "$IP"`.
- NEVER print `$IP`, never `cat` the badge-ip file, never paste an address, network name, host name, MAC, coordinates, contact names or message text into your report. Save raw output to `$S/wc-*.txt` (chmod 600) and quote only the lines this script asks for. Pipe anything you print through `sed -E 's/([0-9]{1,3}\.){3}[0-9]{1,3}/<ip>/g'`.
- After ANY reboot wait at least 75 s before the next console line (poll `ota-info` until it answers, then wait out the 75 s).
- Before any key: `idle test 0`, then `ui lock on 600`. When done: `ui open home`, `ui lock off`.

## Hard limits
- Allowed keys: `up down left right esc wait`, and `enter` ONLY where this script says so, always behind a focus guard (`keys send focus:<Row>| enter`). If a guard fails or a reply says a key is hand-only, stop that step and report it; do not find another way.
- On air: exactly THREE sends to the test channel: one in step C and two in step E. No other `mesh send`, no `dm`, `advert`, `trace`, `discover`, `telemetry`, `remote`, `history <target>`, no Enter on Advertise, Region & radio, TX power, Custom radio, Airplane, Wi-Fi off. If badge.sh or the badge refuses the send, report the refusal text; do not retry another way.
- No flashing, no `ota-firmware`, `ota-assets`, `ota-c6`, `ota-file`, no factory reset, no `notify clear`, no power off, no `fw dev off`, no `fw bl`, no `fw psram speed`, no serial or USB console, no ssh, nothing else on the flash host.
- No edits in the repository, no git, no gh. Your only writes are files under `$S/`.
- If the badge stops answering for more than 4 minutes, or `crash` reports a new crash for this image, STOP the script and report at once with what you have.

Extra rules for this script only:
- Step E runs `sd format ERASE` ONCE. The owner has decided that losing the message history on the SD card is acceptable. Nothing else may be formatted, wiped or reset.
- Step D works on a page that shows the owner's saved position. NEVER copy a coordinate into your report, a file name or a command; never type a valid latitude or longitude; when you save `ui` output or look at a screenshot of that page, report only what the script asks.

## A. Health
1. `tools/badge.sh ota-info "$IP"` -> starts with `badge_fw fw-f742e4ce`.
2. `slot` -> running `(valid)`; `c6` -> `IDENT match`, `up | restarts 0`; `crash` -> `this boot is clean`; `dev` -> `developer mode on`; `airplane` -> `bluetooth off`.
3. `mem` -> dma free >= 89981, min >= 80650; record dma free, dma min, internal free. (Before: 155875 / 149291 / 182087.)
4. `mesh` -> `identity: ok`, `(Netherlands)`; record rx, tx, failed.

## B. #410 fw mem window
1. `mem window` -> three lines starting `mem window internal`, `mem window dma`, `mem window psram`, each with `min`, `age_ms`, `boot_min`, `resets`, `exact`, `sample_ms 250`; each `min` >= its `boot_min`. Quote the three lines.
2. `mem window reset` -> `age_ms 0` (or a few ms), `resets` one higher.
3. Load: `stress 1` if `help` lists a `stress` command that takes a duration in minutes (read its usage first; if unsure, skip the load and say so); otherwise take three screenshots in a row as load. Then `mem window` -> `min` lower than or equal to step 2's, `boot_min` unchanged unless a new since-boot low was hit (then `exact 1`).
4. `mem` -> still prints its normal `mem internal|dma|psram free ...` lines.

## C. #408 transmit air log (one send)
1. `mesh airlog 10` -> a header line `air log: N entries, newest first, M logged since boot, K dropped` (K must be 0) then lines like `#58 t=1240 rx advert flood hops=3 len=90 rssi=-87.4 snr=6.5` or `... tx <kind> ... result=ok`. Count the `tx` lines and compare with `tx` from A4 (only `result=ok` lines count). If there is a `tx` line already: quote it (kind, result): that is the automatic transmit after a start.
2. `mesh send #badgetest wave-C bench 1 <HHMM UTC>`; record the UTC second; wait 20 s; `mesh airlog 5` -> the newest `tx` line is a group text, flood, `retry=0 result=ok`. Quote it. No line may contain a name or an id.
3. `mesh` -> tx one higher than A4, failed unchanged.

## D. #405 remote keys on Advert & contacts (also settles #398)
`idle test 0`, `ui lock on 600`, `ui open meshcore`; `ui` must show `focus: Chat`. `keys send down down right wait focus:Settings| enter`. Step `keys send down` one at a time (read `ui` after each) to `focus: Advert & contacts`, then `keys send focus:Advert & contacts| enter`. Step down to `focus: Latitude`.
1. Gate first: `keys send focus:Latitude| enter` -> the reply must NOT be a hand-only refusal now (the field opens). Then `keys send enter` WITHOUT typing anything. Expected: nothing is saved; the log (fetch it, `wc-log-d.txt`) has a line containing `saving a position takes a hand`. Quote that line (it holds no coordinate). `ui` -> back on the page, focus Latitude.
2. `keys send focus:Latitude| enter`, then `keys send esc` -> back on the page.
3. Invalid value: `keys send focus:Latitude| enter`; `keys send bksp bksp bksp bksp bksp bksp bksp bksp bksp bksp bksp bksp bksp bksp`; `keys send text:95`; `keys send enter`. Screenshot `wc-lat-note.png` and look at it: a red note `latitude is -90 to 90` must be visible on screen under the Longitude row, with the Latitude row still visible. Report only: note visible yes/no, fully readable yes/no. (If the backspaces did not clear the field, the value is still refused as hand-only; then report which note or log line appeared instead.)
4. `ui overflow` -> 0.
5. Hand-only rows: step down to `focus: Advert every`... it is ABOVE Latitude: use `keys send up` one at a time to `focus: Advert every`, then `keys send focus:Advert every| enter` -> expected: a refusal (quote the reply to the key line). Do not try any other row.
6. `ui open home`, `ui lock off`.

## E. #368 journal after an SD format (two sends, one format)
1. `mesh hist` -> history on, record the record count N.
2. `sd format ERASE` -> wait until it answers; it should report 0 errors after the remount. Quote the reply's last line.
3. `mesh send #badgetest wave-C bench 2 <HHMM UTC>`; wait 20 s; fetch the log (`wc-log-e.txt`) -> a line `journal: log.bin was gone (card formatted or wiped): started a new one`, and NO line `journal write of ... failed`. Quote the first.
4. `mesh hist` -> still `history on`; record the count (expected small: head, chat, message).
5. `mesh send #badgetest wave-C bench 3 <HHMM UTC>`; wait 20 s; `mesh hist` -> count higher.
6. `tools/badge.sh ota-reboot "$IP"`; wait 75 s; `mesh hist` -> history on with the records; log has no journal warning or error line (quote any that appear).

## F. Two more reboots and close
1. Twice: `tools/badge.sh ota-reboot "$IP"`, wait 75 s, then `slot`, `c6`, `crash`, `mesh` -> valid, IDENT match, clean, identity ok.
2. `mem` -> record dma free / min, internal free (compare with A3). `dev` -> on. `ui` -> stack `0`, `ui lock: off`.

## Report (to the main session, under 400 words)
A table: step, pass / fail / not run, the one figure or line asked for. Then FINDINGS with the exact reply line (redacted) and the file under `$S/` holding the full output. Then before/after memory. Say plainly if a step was skipped and why. Do not interpret a failure as acceptable; report it.
