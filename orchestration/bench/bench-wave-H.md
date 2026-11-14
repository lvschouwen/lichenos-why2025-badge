# Bench script: wave H on fw-e93ddf36

Wave H = #426 (MeshCore reset not followed by a save; owner-only proof, NOT here), #423 per-app console prefix, #231 app lifecycle, #421 test commands behind developer mode, plus the open ends #420 (list-off checks), #368 (SD format proof, the owner allowed the ONE format line below) and #425 (slow MeshCore loop). Goal: a brief proof on real hardware. Remote only. File prefix for your output: `wh-`.

The main session has updated the badge over Wi-Fi (C6 bundle first, then the P4 image) and saved the log after the first boot (`$S/wh-log-firstboot.txt`). You do NOT flash or push an image.

## Tools (use exactly these)
- `S=/tmp/claude-1000/-home-lucas-coding-why2025-badge/89372c10-2cb8-4883-b9cf-11644d359456/scratchpad`
- Console line over Wi-Fi: `"$S/fw.sh" "<line>" ["<line>" ...]` (lines without the `fw ` prefix; it prints `>> line` and the redacted reply).
- Other badge.sh commands need the address: `IP=$(cat "$S/badge-ip")`, then from /home/lucas/coding/why2025-badge: `tools/badge.sh ota-info "$IP"`, `tools/badge.sh log "$IP" > "$S/wh-log-N.txt"`, `tools/badge.sh screenshot "$IP" "$S/wh-N.png"`, `tools/badge.sh ota-reboot "$IP"`.
- NEVER print `$IP`, never `cat` the badge-ip file, never paste an address, network name, host name, MAC, coordinates, contact names or message text into your report. Save raw output to `$S/wh-*.txt` (chmod 600) and quote only the lines this script asks for. Pipe anything you print through `sed -E 's/([0-9]{1,3}\.){3}[0-9]{1,3}/<ip>/g'`.
- After ANY reboot wait at least 75 s before the next console line (poll `ota-info` until it answers, then wait out the 75 s). Never use a `pgrep` loop; use `sleep` inside one foreground command.
- Before any key: `idle test 0`, then `ui lock on 600`. When done: `ui open home`, `ui lock off`.

## Hard limits
- Allowed keys: `up down left right esc wait`; no `enter` anywhere in this script.
- On air: the ONLY sends allowed are the two test-channel messages of step E (`mesh send #badgetest ...`). No `dm`, `advert`, `trace`, `discover`, `telemetry`, `remote`, `history <target>`. The line in step C3 is expected to be refused by the host guard BEFORE it is sent; if it is not refused, STOP and report.
- Destructive: exactly ONE `sd format ERASE` in step E2 (the owner allowed it; the card's message history is lost on purpose). No other `sd format`, no `factory-reset` line, no `setup reset`, no `notify clear`, no power off, no `dev off`, no `bl`, no `psram speed`, no `trial`, no `onair` line other than the plain `onair` status, no `channel` line.
- Updates: no `ota-firmware`, `ota-assets`, `ota-file`. Step F allows `slot other` (twice); no `ota-c6`.
- No serial or USB console, no ssh, nothing on the flash host. No edits in the repository, no git, no gh. Your only writes are files under `$S/`.
- If the badge stops answering for more than 4 minutes, or `crash` reports a crash for this image, STOP the script and report at once with what you have.

## A. Health
1. `tools/badge.sh ota-info "$IP"` -> starts with `badge_fw fw-e93ddf36`.
2. `slot` -> running `(valid)`; `c6` -> `IDENT match`, `up | restarts 0`; `crash` -> `this boot is clean`; `dev` -> `developer mode on`.
3. `mem` -> dma free >= 89981, min >= 80650; record dma free, dma min, internal free, psram free. (Wave G: about 156 KB free, 146-149 KB min.)
4. `mesh` -> `identity: ok`; record rx, tx, failed and the contact count. `onair` -> `on-air allow list off`.
5. `soak` -> record `boot`, `crash`, `rstcrash` (expected 0), `mslow`, `mslowms`.
6. `boot` -> all rows done; record the `apps` step time (1.5 s on the last image; flag more than 1.7 s).

## B. #231 app lifecycle
1. `app life` -> one line per app in the form `app life <id>: <state>, fault <f>, <n> timers, <n> stops (last <reason>, <ms> ms, <n> tasks left)` and a last line `app life: <N> apps`. After a fresh boot: every app `registered` or `background`, fault `-`, `0 stops (last -`. Save to `wh-life-1.txt`; quote the summary line, the meshcore line and any line whose fault is not `-`.
2. `mem` -> for every app with a PSRAM quota a line `mem app <id> psram quota <n> (accounted) passed <k> time(s)`. Quote those lines. `(enforced)` anywhere is a finding.
3. `ui open timer`; `app life` -> timer is `open`. `ui open home`; `app life` -> timer is `suspended` or back to `registered`/`background` (quote the line; report which). Repeat with `ui open notes` (or another small app from the `app life` list).
4. `ui perf` -> render p95; flag more than 7 ms (6 ms on record).
5. Restart stop: `reboot`; wait for `ota-info`, then 75 s. Log (`wh-log-b5.txt`, taken BEFORE the reboot is not possible, so read `log` right after the wait and look at the previous-boot section if the log keeps one): quote every line of the stop path naming an app as late or leaving tasks or memory. None is the expected result; a clean stop logs nothing.
6. After the reboot: `app life` -> fresh state again (0 stops: the records do not survive a reboot); `mesh` -> `identity: ok`, the same contact count as A4; `crash` -> clean.

## C. #423 per-app console prefix
1. `help` > `wh-help.txt` -> the OS commands, then a group headed `meshcore (also: fw meshcore <command>):` and a `qrcode` group; last line `N of 96 commands registered` (54 before #421; record N).
2. `meshcore mesh` -> the same status lines as `mesh` (compare the first three lines with the numbers taken out). `meshcore qr x` -> `unknown command`. `qrcode mesh` -> `unknown command`.
3. `"$S/fw.sh" "meshcore mesh send #not-the-test-channel x"` -> refused by the host guard before it is sent (quote the refusal). Then `mesh` -> tx unchanged against A4.

## D. #421 test commands behind developer mode (developer mode is ON here; the OFF half is in the owner's hands sitting)
1. With developer mode on, the gated commands still answer: `idle test 0` -> accepted; `notify test info` -> a test notice appears in `notify`; `thermal`, `psram`, `sensors`, `stress status` (plain status forms) -> each answers normally. `meshcore psram test 64` -> `unknown command, try 'fw help'`. Usage text: `sensors x` -> usage lists `start|stop|boot [on|off]`; `idle x` -> usage lists `idle test <seconds>`.
Do NOT run `stress`, `noscreen`, `setup reset`, `psram <anything with an argument>`, `thermal test`, `vib test` or `sensors boot|stop|start`.

## E. #368 SD format while MeshCore runs (the owner allowed the one format)
1. `mesh send #badgetest wave-H before-format`; wait 20 s; `mesh hist` -> `history on`; record the record count.
2. **`sd format ERASE`** (ONCE) -> quote the reply; expected: errors 0 after the remount. If it is refused, report the refusal and skip to F; do not find another way.
3. `mesh send #badgetest wave-H after-format`; wait 20 s. Log (`wh-log-e.txt`): a line `journal: log.bin was gone (card formatted or wiped): started a new one` (quote) and NO line `journal write of ... failed`.
4. `mesh hist` -> `history on`, 3 records (head, chat, message); quote the counts only.
5. `reboot`; wait for `ota-info`, then 75 s. `mesh hist` -> history on; the log has no journal warning. `sd` -> errors 0.

## F. #420 list-off checks (the list is off; the owner switched it off at USB)
1. `onair` -> `on-air allow list off`.
2. `slot` -> record the running slot X. `slot other` -> quote the reply; `reboot`; wait for `ota-info`, then 75 s. `slot` -> running the other slot; `tools/badge.sh ota-info "$IP"` -> record its label (the previous image).
3. `slot other`; `reboot`; wait, 75 s. `slot` -> running X `(valid)`; `ota-info` -> `fw-e93ddf36`.
4. (Done by main before the update: `ota-c6` was accepted with the list off, no warning, the C6 came back `IDENT match`, restarts 0.) `c6` -> `IDENT match`, `up`; record `restarts` (0 or 1 after the slot switches is fine; more is a finding).
5. `mesh` -> `identity: ok`, rx rising over 60 s (record both).

## G. #425 slow MeshCore loop (passive, do this last and let it run)
1. Wait until 10 minutes after the last reboot. Log (`wh-log-g.txt`): quote every `contacts saved after the unlock:` line and every `MeshCore loop took` line (numbers only, they hold no private data).
2. `soak` -> `mslow`, `mslowms`, `mslowfile`. Expected: `mslow=0`.

## H. Close
1. `slot` -> running `(valid)` on `fw-e93ddf36`; `c6` -> IDENT match; `crash` -> clean, 0 records for this image; `dev` -> on; `onair` -> off.
2. `mem` -> dma free / min, internal free, psram free (gate free >= 89981, min >= 80650); compare with A3.
3. `soak` -> quote `soak1` up to `slot=`. `boot` -> all rows done. `ui` -> stack `0`, `ui lock: off`.
4. `app life` -> quote the summary line and any line with a fault other than `-`.

## Report (to the main session, under 500 words)
A table: step, pass / fail / not run, the one figure or line asked for. Then FINDINGS with the exact reply line (redacted) and the file under `$S/` holding the full output. Then before/after memory. Say plainly if a step was skipped and why. Do not interpret a failure as acceptable; report it.
