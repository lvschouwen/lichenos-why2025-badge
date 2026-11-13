# Bench script: wave A on fw-0746e922

Wave A = Round 1 (#389 part, #368, #397, #211, #208) + Round 2 (#11 wave 1, #212, #298). The badge was just updated to `fw-0746e922`. Goal: a brief proof on real hardware that the wave boots, runs and hits no bugs. Remote only.

## Tools (use exactly these)
- `S=<scratchpad>`
- Console line over Wi-Fi: `"$S/fw.sh" "<line>" ["<line>" ...]` (lines without the `fw ` prefix; it prints `>> line` and the redacted reply).
- Other badge.sh commands need the address: `IP=$(cat "$S/badge-ip")`, then from <repo>: `tools/badge.sh ota-info "$IP"`, `tools/badge.sh log "$IP" > "$S/wa-log-N.txt"`, `tools/badge.sh screenshot "$IP" "$S/wa-N.png"`, `tools/badge.sh ota-reboot "$IP"`.
- NEVER print `$IP`, never `cat` the badge-ip file, never paste an address, network name, host name, MAC, coordinates, contact names or message text into your report. Save raw output to `$S/wa-*.txt` (chmod 600) and quote only the lines this script asks for. Pipe anything you print through `sed -E 's/([0-9]{1,3}\.){3}[0-9]{1,3}/<ip>/g'`.
- After ANY reboot wait at least 75 s before the next console line (poll `ota-info` until it answers, then wait out the 75 s).
- Before any key: `idle test 0`, then `ui lock on 600`. When done: `ui open home`, `ui lock off`.

## Hard limits
- Allowed keys: `up down left right esc wait`, and `enter` ONLY where this script says so, always behind a focus guard (`keys send focus:<Row>| enter`). If a guard fails or a reply says a key is hand-only, stop that step and report it; do not find another way.
- On air: exactly ONE send, the one in step G. No other `mesh send`, no `dm`, `advert`, `trace`, `discover`, `telemetry`, `remote`, `history <target>`, no Enter on Advertise, Region & radio, TX power, Custom radio, Airplane, Wi-Fi off. If badge.sh or the badge refuses the send, report the refusal text; do not retry another way.
- No flashing, no `ota-firmware`, `ota-assets`, `ota-c6`, `ota-file`, no `fw sd format`, no factory reset, no `notify clear`, no power off, no `fw dev off`, no `fw bl`, no `fw psram speed`, no serial or USB console, no ssh, nothing else on the flash host.
- No edits in the repository, no git, no gh. Your only writes are files under `$S/`.
- If the badge stops answering for more than 4 minutes, or `crash` reports a new crash for this image, STOP the script and report at once with what you have.

## A. Health and the two-chip gate (remote parts)
1. `tools/badge.sh ota-info "$IP"` -> starts with `badge_fw fw-0746e922`; note whether the receiver words include `zpieces`.
2. `slot` -> `running ... (valid)`; other slot `fw-4bda782a`.
3. `c6` -> `IDENT match`, `c6: up | restarts 0`.
4. `boot` -> every step `done`; record the `apps` end time (ms) and whether a `battery` step is listed.
5. `crash` -> `this boot is clean`; no record naming `fw-0746e922`.
6. `mem` -> `mem dma free` >= 89981 and `min` >= 80650; record both, plus `mem internal free`. (Before this wave: free 156183, min 148571.)
7. `airplane` -> `... bluetooth off, controller idle ...`.
8. `mesh` -> `identity: ok`, the radio line ends `(Netherlands)`; record `rx N`. Wait 90 s, `mesh` again -> rx higher or equal (say which; equal is not a failure on a quiet channel).
9. `tasks` -> record whether a `battery` task is listed and on which memory (expect LP RAM); list any Lichen task without an owner (informational).
10. `dev` -> `developer mode on`.

## B. #11 battery service, no gauge fitted
1. Save the log (`wa-log-1.txt`). `grep -c -i 'battery' ` and print the battery lines. Expected: exactly one line, saying nothing answers at 0x36 (`no fuel gauge ... nothing at 0x36` or similar). A line saying `SDA/SCL held low` or `bus not available` is a FINDING (wrong pin mapping): quote it.
2. `battery` -> `absent (probe: nothing at 0x36)` or similar; record the stack-free figure.
3. `battery probe` three times, 5 s apart -> same result each time, no error lines in the log.
4. `i2c scan` -> lists 0x34, 0x69 and 0x76 as before.

## C. #208 mesh task split (console output must be unchanged)
Save every reply to `$S/wa-mesh.txt`; in the report give only pass/fail per line and the first line of any unexpected reply.
1. `help` -> has usage lines for `mesh`, `contacts`, `msgs`, `channels`, `channel`, `advert`.
2. Each of these answers without `unknown`, `error` or a crash: `mesh link`, `mesh out`, `mesh chats`, `mesh scope`, `mesh radio`, `mesh pathhash`, `mesh autostart`, `mesh hist`, `mesh notify`, `mesh order`, `contacts`, `msgs 5`, `channels`.
3. `mesh bogus` -> the usage text (not a crash, not silence).
4. `mesh stop`; then `mesh`, `mesh link`, `mesh bogus` -> each gives the "MeshCore ... not running" style reply; `mesh hist`, `mesh notify`, `mesh autostart` -> their normal replies.
5. `mesh start`; wait 15 s; `mesh` -> identity ok, radio line present.

## D. #212 LoRa task and C6 link
1. `lora` -> state RX, config line present; record the packet counter.
2. `mem` (whole reply to `wa-mem.txt`) -> find the LoRa task line; stack free >= 25 % of its size. Record size and free.
3. In `wa-log-1.txt`: the C6 boot plan line (grep `c6`) gives a reason; quote that one line.
4. C6 reset: `c6` (note `restarts`), `c6 reset`. The Wi-Fi drops. Wait 75 s (poll `ota-info`). Then `c6` -> `up`, `restarts` one higher, `IDENT match`; `mesh` -> radio line present; `lora` -> state RX. Save the log (`wa-log-2.txt`) and quote the lines containing `C6 lost` and `C6 answers again` (or say they are absent).

## E. #298 / #211 update receiver
1. From A1: `zpieces` present in `ota-info` (the new receiver advertises compressed pieces).
2. `crash` still clean after the earlier steps. (The compressed push itself is proven by the next wave's update; do not push anything.)

## F. #397 Display page
1. `idle test 0`, `ui lock on 600`, `ui open settings`.
2. Step `keys send down` one at a time, reading `ui` after each, until `focus: Display` (it was 6 downs). Then `keys send focus:Display| enter`.
3. `ui` -> title `Display`; rows in this order: Screen, Keyboard, (now notes), Message light, Flashlight, Dim after, Screen off after, Clock screensaver, Screensaver style, Pocket lock, Home layout, Arrange home, Reset order. Use `keys send down` to bring later rows into the dump; never `enter`, `left` or `right` on this page.
4. Screenshot (`wa-display.png`), look at it: no clipped or overlapping text.
5. `ui overflow` -> 0.
6. `ui open home`, `ui lock off`.

## G. One on-air send (#389 "sent" label, #212, #208)
1. `mesh scope` -> note the console scope line. `mesh out` -> note the count.
2. `mesh send #badgetest wave-A bench <HHMM UTC>` (ONE time). Record the UTC time to the second.
3. Poll `mesh out` every 5 s for 60 s. Record the sequence of states for that message (expected: queued and/or sent, then on air, then heard repeated or a final state). Any state text is fine to quote; do not quote other messages.
4. `mesh` -> `tx` one higher than in A8, `failed 0`.

## H. Reboot loop (boot order changed in #11)
Ten times: `tools/badge.sh ota-reboot "$IP"`, wait until `ota-info` answers and 75 s have passed, then `slot`, `c6`, `crash`, `battery`. Record per round: running slot and `(valid)`, `IDENT match`, `this boot is clean`, battery state. Stop at the first round that is not clean and report it.

## I. Close
1. `mem` -> record dma free / min and internal free again (compare with A6).
2. `crash` -> clean. `dev` -> on. `ui` -> stack `0`, `ui lock: off`.

## Report (to the main session, under 450 words)
A table: step, pass / fail / not run, the one figure or line asked for. Then FINDINGS: anything unexpected, each with the exact reply line (redacted) and the file under `$S/` that holds the full output. Then the before/after memory figures. Say plainly if a step was skipped and why. Do not interpret a failure as acceptable; report it.
