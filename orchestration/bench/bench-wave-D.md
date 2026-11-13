# Bench script: wave D on fw-869e608d

Wave D = #288 app secrets with a one-time password move, #411 Wi-Fi trial rule, #412 status poll timing, #409 `fw soak`; plus a block of older unproven issues (step H). Goal: a brief proof on real hardware. Remote only. File prefix for your output: `wd-`.

The main session has already flashed the image, saved the first-boot log (`$S/wd-log-firstboot.txt`) and run the #411 update steps. You do NOT update, flash or roll back anything.

## Tools (use exactly these)
- `S=<scratchpad>`
- Console line over Wi-Fi: `"$S/fw.sh" "<line>" ["<line>" ...]` (lines without the `fw ` prefix; it prints `>> line` and the redacted reply).
- Other badge.sh commands need the address: `IP=$(cat "$S/badge-ip")`, then from /home/lucas/coding/why2025-badge: `tools/badge.sh ota-info "$IP"`, `tools/badge.sh log "$IP" > "$S/wd-log-N.txt"`, `tools/badge.sh screenshot "$IP" "$S/wd-N.png"`, `tools/badge.sh ota-reboot "$IP"`.
- NEVER print `$IP`, never `cat` the badge-ip file, never paste an address, network name, host name, MAC, coordinates, contact names or message text into your report. Save raw output to `$S/wd-*.txt` (chmod 600) and quote only the lines this script asks for. Pipe anything you print through `sed -E 's/([0-9]{1,3}\.){3}[0-9]{1,3}/<ip>/g'`.
- After ANY reboot wait at least 75 s before the next console line (poll `ota-info` until it answers, then wait out the 75 s). Never use a `pgrep` loop; use `sleep` inside one foreground command.
- Before any key: `idle test 0`, then `ui lock on 600`. When done: `ui open home`, `ui lock off`.

## Hard limits
- Allowed keys: `up down left right esc wait`, and `enter` ONLY where this script says so, always behind a focus guard (`keys send focus:<Row>| enter`). If a guard fails or a reply says a key is hand-only, stop that step and report it; do not find another way.
- On air: NOTHING. No `mesh send`, `dm`, `advert`, `trace`, `discover`, `telemetry`, `remote`, `history <target>`, no Enter on Advertise, Region & radio, TX power, Custom radio, Airplane, Wi-Fi off. If badge.sh or the badge refuses a line, report the refusal text; do not retry another way.
- No flashing, no `ota-firmware`, `ota-assets`, `ota-c6`, `ota-file`, no `sd format`, no `factory-reset` line other than the exact `factory-reset plan all` in step B, no `notify clear`, no power off, no `dev off`, no `bl`, no `psram speed`, no `trial`, no serial or USB console, no ssh, nothing on the flash host.
- No edits in the repository, no git, no gh. Your only writes are files under `$S/`.
- If the badge stops answering for more than 4 minutes, or `crash` reports a crash for this image other than the one step F makes, STOP the script and report at once with what you have.

## A. Health
1. `tools/badge.sh ota-info "$IP"` -> starts with `badge_fw fw-869e608d`.
2. `slot` -> running `(valid)`; `c6` -> `IDENT match`, `up | restarts 0`; `crash` -> `this boot is clean`; `dev` -> `developer mode on`; `airplane` -> `bluetooth off`.
3. `mem` -> dma free >= 89981, min >= 80650; record dma free, dma min, internal free. (Wave C: about 156 KB / 146-149 KB.)
4. `mesh` -> `identity: ok`, `(Netherlands)`; record rx, tx, failed. `mesh history` -> `off`.

## B. #288 app secrets
1. In `$S/wd-log-firstboot.txt` (already saved by main): exactly one line containing `app secrets:`; quote it (it holds counts only). Expected form: `app secrets: N moved into the secrets class, 0 invalid dropped, 0 not moved`.
2. Fetch the current boot's log (`wd-log-b.txt`); if this boot is NOT the first boot after the flash (main tells you in the brief), it must hold NO `app secrets:` line. Report the count of such lines.
3. `factory-reset plan all` > `$S/wd-plan.txt` (chmod 600; it lists setting names, do not paste it). Quote ONLY the line containing `app secret(s), names not shown`; N should equal the moved count of B1 (or be higher). Also report: does any line of the plan contain `pass` or `pwd` in a key name (yes/no, count only).

## C. #412 MeshCore start with the air log (polling is off: no transmit expected)
1. `mesh stop`; wait 10 s; `mesh start`; wait 15 s.
2. `mesh airlog 10` -> header `air log: N entries, newest first, M logged since boot, K dropped`, K = 0. Count the `tx` lines newer than the start: expected 0 (the badge does not transmit by itself with polling off). Quote any `tx` line (kind and result only).
3. `mesh` -> `identity: ok`, `failed` unchanged from A4, tx unchanged.

## D. #409 fw soak
1. `soak` three times, 20 s apart; save each reply. Each reply: exactly three lines `soak1 ...`, `soak2 ...`, `soak3 ...`; within one reply `v`, `boot` and `up` are identical on the three lines; `up` rises from reply to reply; `boot` the same in all three. No names, addresses or free text in any line. Quote the first reply's `soak1` line and record `boot` = B0, `rst`, `rstcrash`.
2. Record the mesh keys of the soak lines (`mesh_rx`, `mesh_tx`, `mesh_failed` or whatever the lines call them): they are >= 0 while MeshCore runs.
3. `mesh stop`; wait 5 s; `soak` -> every mesh key reads `-1`. Quote that line. `mesh start`; wait 15 s; `soak` -> the mesh keys are >= 0 again.

## E. #409 restart
1. `reboot` (the console command); wait until `ota-info` answers, then 75 s.
2. `soak` -> `boot` = B0 + 1, `rst=sw`, `rstcrash=0`. Quote `soak1`.
3. Log of this boot (`wd-log-e.txt`): count of `app secrets:` lines must be 0 (#288: the move line is gone after a reboot).
4. `slot`, `c6`, `crash`, `mesh` -> valid, IDENT match, clean, identity ok.

## F. #409 crash counter
1. `crash` -> record the number of records.
2. `crash test panic`; wait until `ota-info` answers again, then 75 s.
3. `soak` -> `boot` = B0 + 2, `rstcrash=1`; quote `soak1`. `crash` -> one record more, naming this image, reason panic (the test).
4. `crash clear`; `crash` -> no record for this image. `soak` -> report what `rstcrash` reads now (informational).
5. `slot` -> still running `(valid)` on `fw-869e608d`; `c6` -> IDENT match; `mesh` -> identity ok.

## G. #411 slot other (console choice needs no Wi-Fi rule)
1. `slot` -> record both slots and which runs. If the other slot is not `(valid)` or does not hold `fw-869e608d` or the previous wave image `fw-f742e4ce`, SKIP step G and say so.
2. `slot other`; wait until `ota-info` answers, then 75 s. `ota-info` -> the other image's label. Log (`wd-log-g1.txt`) -> a line containing `chosen at the console`; quote it. `slot` -> the running slot `(valid)` (validation comes at about 45 s of uptime).
3. `slot other` again; wait as before. `ota-info` -> `badge_fw fw-869e608d`; `slot` -> running `(valid)`; log (`wd-log-g2.txt`) -> the `chosen at the console` line again.
4. `dev` -> `developer mode on`.

## H. Older issues (remote steps compiled by the prep agent)
No Enter anywhere in step H except the two guarded ones named in H9. No sends. One key per call in lists, `ui` after each. Report each as its own row with the issue number.
1. **#205** `mem`, `ui perf` -> each `mem` line ends with `total N free_pct .. min_pct .. largest_pct .. used_pct ..` (if the format differs, quote what it prints); the `ui perf latency:` line ends `first frame at <ms> ms`, under 3000. Quote the `mem internal` line and the latency line.
2. **#261** `mem blocks internal 4096`, `mem blocks internal 1024`, `ui perf bench 10` -> block counts no worse than 22 blocks / 167704 B and 38 blocks / 199292 B; the bench finishes; the log has no heap or canary line. Quote both `blocks` lines and the bench result.
3. **#263** `sd test`, `mem` -> `sd test ok: N KB written at X MB/s, read back at Y MB/s` (X near 2.3, Y near 4.3). Quote it.
4. **#256** `psram dma` -> `psram dma: 0 bad bytes in 1 round(s), 0 errors`; from `mem`, the dma min and the newest `dma_low drop` line if any (record only).
5. **#210** `boot` (after the reboots of E, F and G) -> every row DONE; quote the row count line.
6. **#236** `ui open settings`; step down to About (read `ui` after each key); `ui`; `ui overflow` -> heading `Lichen OS 2`, version row `Lichen OS 2 (fw-869e608d)`, overflow 0, hint `Up/Down: scroll`.
7. **#136 / #138** still in Settings: read `ui` (no Enter) with the focus on Power, Battery, Files, Notifications; where a page must be opened to read it, use `ui open <app>` only if `help`/`ui open` lists that page, else read the row labels only. Check: sentence-case rows, hints with ` • `, a degree sign in temperatures, `ui overflow` 0 on each screen read. Quote the overflow lines. Never Enter on Power.
8. **#127** `ui open meshcore`; go to Contacts with keys (no Enter on a contact); `keys send down` x8 and `keys send up` x4, each followed by `ui` -> focus moves exactly one row per key; `keys send left right left right` -> focus unchanged; `ui overflow` 0. Report focus positions as row indexes only, NEVER a contact name.
9. **#52 / #50** from the MeshCore grid: `keys send down down right wait focus:Settings| enter`; step down to `focus: Advert & contacts`; `keys send focus:Advert & contacts| enter`; `ui` twice 10 s apart -> rows `Advert every`, `Next advert`, `Advert button sends`, `Latitude`, `Longitude` exist and `Next advert` is not blank (report row NAMES only, never the position values). `keys send esc`. Step to `focus: Notifications`; `keys send focus:Notifications| enter`; `ui` -> heading `MeshCore notifications`, rows `Direct messages`, `Delivery failed`, `Mentions of my name` (report the three fixed rows only, not channel names). `keys send esc esc`. Then `mesh autoadd` -> `overwrite oldest when full: on|off`; `mesh notify` -> counters (numbers only).
10. **#133** from the repo: `tools/ui_overflow_sweep.py "$IP" --list | sed ...redact` and `tools/ui_overflow_sweep.py "$IP" --try home` -> `ok   home: 0 overflowing`. No other `--try`, no plain sweep.
11. `ui open home`, `ui lock off`.

## I. Close
1. `mem` -> record dma free / min, internal free (compare with A3; gate free >= 89981, min >= 80650). `mem window` -> quote the dma line.
2. `crash` -> clean for this image. `dev` -> on. `mesh` -> identity ok, `(Netherlands)`, tx equal to A4 (nothing was sent). `ui` -> stack `0`, `ui lock: off`.

## Report (to the main session, under 500 words)
A table: step, pass / fail / not run, the one figure or line asked for. Then FINDINGS with the exact reply line (redacted) and the file under `$S/` holding the full output. Then before/after memory. Say plainly if a step was skipped and why. Do not interpret a failure as acceptable; report it.
