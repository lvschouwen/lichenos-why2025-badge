# Bench script: wave E on fw-73532714

Wave E = #416 sticky `rstcrash`, #403 OTA parser follow-ups, #289 on-air allow list (remote part), #232 crash attribution and safe mode. Goal: a brief proof on real hardware. Remote only. File prefix for your output: `we-`.

The main session has flashed the image over USB and saved the first-boot log (`$S/we-log-firstboot.txt`). You do NOT update, flash or roll back anything.

## Tools (use exactly these)
- `S=<scratchpad>`
- Console line over Wi-Fi: `"$S/fw.sh" "<line>" ["<line>" ...]` (lines without the `fw ` prefix; it prints `>> line` and the redacted reply).
- Other badge.sh commands need the address: `IP=$(cat "$S/badge-ip")`, then from /home/lucas/coding/why2025-badge: `tools/badge.sh ota-info "$IP"`, `tools/badge.sh log "$IP" > "$S/we-log-N.txt"`, `tools/badge.sh screenshot "$IP" "$S/we-N.png"`, `tools/badge.sh ota-reboot "$IP"`.
- NEVER print `$IP`, never `cat` the badge-ip file, never paste an address, network name, host name, MAC, coordinates, contact names or message text into your report. Save raw output to `$S/we-*.txt` (chmod 600) and quote only the lines this script asks for. Pipe anything you print through `sed -E 's/([0-9]{1,3}\.){3}[0-9]{1,3}/<ip>/g'`.
- After ANY reboot wait at least 75 s before the next console line (poll `ota-info` until it answers, then wait out the 75 s). Never use a `pgrep` loop; use `sleep` inside one foreground command.
- Before any key: `idle test 0`, then `ui lock on 600`. When done: `ui open home`, `ui lock off`.

## Hard limits
- Allowed keys: `up down left right esc wait`, and `enter` ONLY where this script says so, always behind a focus guard (`keys send focus:<Row>| enter`). If a guard fails or a reply says a key is hand-only, stop that step and report it; do not find another way.
- On air: NOTHING is expected to go out. The ONLY send line allowed is the one in step F3 (to the test channel, expected to be refused by the badge). No other `mesh send`, no `dm`, `advert`, `trace`, `discover`, `telemetry`, `remote`, `history <target>`, no Enter on Advertise, Region & radio, TX power, Custom radio, Airplane, Wi-Fi off. If badge.sh or the badge refuses a line, report the refusal text; do not retry another way.
- No flashing, no `ota-firmware`, `ota-assets`, `ota-c6`, `ota-file`, no `sd format`, no `factory-reset` line other than the exact `factory-reset plan all` in step F4, no `notify clear`, no power off, no `dev off`, no `bl`, no `psram speed`, no `trial`, no `onair` line other than the exact `onair` and `onair on` of step F, no `channel` line, no `slot other`, no serial or USB console, no ssh, nothing on the flash host.
- No edits in the repository, no git, no gh. Your only writes are files under `$S/`.
- If the badge stops answering for more than 4 minutes, or `crash` reports a crash for this image other than the test crashes this script makes, STOP the script and report at once with what you have.

## A. Health
1. `tools/badge.sh ota-info "$IP"` -> starts with `badge_fw fw-73532714`.
2. `slot` -> running `(valid)`; `c6` -> `IDENT match`, `up | restarts 0`; `crash` -> `this boot is clean`; `dev` -> `developer mode on`; `airplane` -> `bluetooth off`.
3. `mem` -> dma free >= 89981, min >= 80650; record dma free, dma min, internal free. (Wave D: 155711 / 149139 / 181907.)
4. `mesh` -> `identity: ok`, `(Netherlands)`; record rx, tx, failed. `onair` -> `on-air allow list off`, `0 of 8 entries`.
5. `app background` -> `off for no app`, `degraded boot no`. Current boot log (`we-log-a.txt`): NO line containing `crash attribution off`.
6. `soak` -> three lines; record `boot` = B0, `crash` = C0, `rstcrash` (expected 0).

After EVERY test crash below: the badge restarts twice (the crash, then about 20 s later the `psram 120 re-arm after crash` restart). Wait until `ota-info` answers, then 100 s, then continue. The `fw.sh` call that triggers a crash may time out: that is expected.

## B. #416 sticky rstcrash
1. `crash test panic`; wait as above. `soak` -> `rst=sw rstcrash=1`, `boot` = B0+2, `crash` = C0+1. Quote `soak1` up to `slot=`.
2. `app background` -> the crash is attributed to no app: `off for no app` (quote the lines).
3. `crash clear`; `soak` -> `crash=0` and `rstcrash` STILL 1 (same boot).
4. `reboot`; wait for `ota-info`, then 75 s. `soak` -> `rstcrash=0`, `rst=sw`.

## C. #232 an app without a background part (timer)
1. `app crash timer`; wait. `notify` -> the newest entry names Timer (quote its title and body; it holds no private data). Log (`we-log-c1.txt`): a line with `attributed to timer` (quote). `app background` -> quote all lines (expected: off for no app, degraded boot yes, and on this boot a note that it is the re-arm restart).
2. `app crash timer` again; wait. `notify` -> newest entry reads like `Timer crashed 2 times in the last 3 boots` (quote). `app background` -> still `off for no app`.

## D. #232 an app with a background part (weather)
1. `tasks` -> a line `task weather owner weather` exists.
2. `app crash weather`; wait. `app crash weather` again; wait.
3. `notify` -> newest entry says its background part is off (quote). `app background` -> lists `weather` (quote). `tasks` -> NO `task weather` line. Log (`we-log-d.txt`): the line saying the background part is off for weather (quote).
4. `app background weather on` -> quote the reply. `reboot`; wait for `ota-info`, then 75 s. `tasks` -> `task weather` is back; `app background` -> `off for no app`.

## E. #232 MeshCore is exempt
1. `app crash meshcore`; wait. `app crash meshcore` again; wait.
2. `notify` -> newest entry names MeshCore with a count, and does NOT say its background part is off (quote title and body). `app background` -> `off for no app`, `degraded boot yes`.
3. `mesh` -> `identity: ok`, `(Netherlands)`; wait 60 s; `mesh` -> rx not lower (record both). `mesh hist` -> `history on`, quote the first line's record counts only.
4. `crash` -> record the count; `crash clear`; `crash` -> 0.

## F. #289 allow list, remote part (leaves the list ON and EMPTY: the owner switches it off at USB afterwards)
1. `onair` -> off, 0 of 8 entries. `mesh` -> record tx = T0.
2. `onair on` -> quote the reply (expected: on, saved, the list is empty, every console send is refused). `onair` -> on, 0 of 8.
3. `mesh send #badgetest wave-E refused-test` -> expected reply contains `on air refused` and `not on the console allow list`; quote it. Wait 15 s. `mesh` -> tx still T0. `mesh airlog 5` -> no new `tx` line. If the badge did NOT refuse (the message was queued), STOP step F, report it as a failure, and send nothing else.
4. `factory-reset plan all` > `$S/we-plan.txt` (chmod 600, do not paste) -> it still answers (report line count only).
5. `reboot`; wait for `ota-info`, then 75 s. `onair` -> still on, 0 of 8 entries (the setting persists).
Do NOT try `onair off`, `onair allow`, `factory-reset ... YES` or anything else here.

## G. Close
1. `slot` -> running `(valid)` on `fw-73532714`; `c6` -> IDENT match, restarts 0; `crash` -> clean, 0 records; `dev` -> on.
2. `mem` -> dma free / min, internal free (gate free >= 89981, min >= 80650); compare with A3.
3. `soak` -> quote `soak1` up to `slot=`; `rstcrash` 0. `boot` -> all rows done. `ui` -> stack `0`, `ui lock: off`.
4. `app background` -> quote all lines.

## Report (to the main session, under 500 words)
A table: step, pass / fail / not run, the one figure or line asked for. Then FINDINGS with the exact reply line (redacted) and the file under `$S/` holding the full output. Then before/after memory. Say plainly if a step was skipped and why. Do not interpret a failure as acceptable; report it.
