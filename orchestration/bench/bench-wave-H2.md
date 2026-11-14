# Bench script: the remote part of the hands sitting, on fw-e93ddf36

The owner allowed these checks to run remotely (2026-11-14), with injected keys including Enter on the rows named here, the deletion of ONE test note and ONE crash-log entry, and Clear all on the notification list. Covers #273, #377 (the allowed half), #274, #229 (Notes and Timer), #264, #286 (Clear all). The #421 developer-mode-off sweep is run by the main session over the USB console, not here. File prefix for your output: `wr-`.

## Tools (use exactly these)
- `S=<the session scratchpad named in your brief>`
- Console line over Wi-Fi: `"$S/fw.sh" "<line>" ["<line>" ...]` (lines without the `fw ` prefix; it prints `>> line` and the redacted reply).
- `IP=$(cat "$S/badge-ip")`, then from /home/lucas/coding/why2025-badge: `tools/badge.sh ota-info "$IP"`, `tools/badge.sh log "$IP" > "$S/wr-log-N.txt"`, `tools/badge.sh screenshot "$IP" "$S/wr-N.png"`, `tools/badge.sh ota-file "$IP" <file> <badge-path>`, `tools/badge.sh ota-rm "$IP" <badge-path>` (the last two only in step D).
- NEVER print `$IP`, never `cat` the badge-ip file, never paste an address, network name, host name, MAC, coordinates, contact names, message text or the text of a notification into your report. Save raw output to `$S/wr-*.txt` (chmod 600) and quote only the lines this script asks for. Look at screenshots yourself; describe them, do not attach them.
- Before any key: `idle test 0`, then `ui lock on 600` (renew it when a step takes longer). When done: `ui open home`, `ui lock off`.
- Reading focus: `ui` shows the stack and `focus: <Row>`. Step ONE key per call in lists and read `ui` after each. Every Enter goes behind a focus guard: `keys send focus:<Row>| enter`. If a guard fails, stop that step.
- Dialogs: `ui answer` shows the open dialog and its verdict; `ui answer yes <confirm_label>` confirms it when the policy allows; `ui answer no` cancels (`ui answer: Esc sent`). `keys send` cannot answer a dialog.

## Hard limits
- Enter ONLY on these rows: `Reset order` (Settings > Display), a note row whose title starts with `bench` (Notes), `Clear all` (Notifications), one entry in the Crash log, the Timer app's own start control, and what step D needs in Files. Nowhere else. Never Enter on anything in MeshCore, Wi-Fi, Power, Region, About, Factory reset, Backup, Arrange home.
- `ui answer yes` ONLY for: `delete` on "Delete this note?", `delete` on "Delete this crash entry? ...", `clear all` on the notification dialog, `delete` on a file named `wr-test.bin`. NEVER `ui answer yes reset` (it would reset the owner's arranged home order): the Reset order dialog is only ever cancelled.
- On air: nothing. No `mesh send`, `dm`, `advert`, `trace`, `discover`, `telemetry`, `remote`, `history <target>`.
- No update, no `slot other`, no `sd format`, no `factory-reset`, no `setup`, no `dev off`, no `noscreen`, no `stress`, no `thermal test`, no `psram` with an argument, no `onair` edit, no power off, no reboot (none is needed).
- No edits in the repository, no git, no gh, no serial or USB console, no ssh.
- If the badge stops answering for more than 4 minutes, or `crash` shows a NEW crash for this image, STOP and report.

## A. Start
1. `tools/badge.sh ota-info "$IP"` -> `badge_fw fw-e93ddf36`. `slot` -> `(valid)`; `dev` -> `developer mode on`; `crash` -> record the entry count C0 and whether this boot is clean.
2. `mem` -> record dma free / min, internal free, psram free. `app caps` -> record the refused count R0 (expected `0 refused since boot`).
3. `home` (the console side of the home layout) -> save to `wr-home-before.txt`; this is the owner's arranged order and must be identical at the end.
4. `idle test 0`, `ui lock on 600`.

## B. #273 and #377: the Reset order dialog (cancel only)
1. `ui open settings`; step down to `focus: Display` (one key per call); `keys send focus:Display| enter`; step to `focus: Reset order`; `keys send focus:Reset order| enter`.
2. `ui answer` -> a line starting `ui answer: ALLOW`, confirm label `reset`, title `Reset the home order? The tiles go back to their first places.` Quote it.
3. `keys send esc` is not possible on a dialog: use `ui answer no` -> `ui answer: Esc sent`. Log (`wr-log-b.txt`): a line with `answered: cancel` (quote).
4. Open the dialog again (`keys send focus:Reset order| enter`); do NOT answer; wait for the countdown to run out (read `ui answer` every 10 s, at most 90 s) -> `ui answer: no dialog open`. Log: a second `answered: cancel`, and NO line `could not be shown (another dialog is opening)`.
5. With no dialog open: `ui answer no` -> `ui answer: no dialog open` (quote the exact reply).
6. `home` -> identical to `wr-home-before.txt` (`diff`); if it differs, STOP and report at once.

## C. #274 and #229: Notes
1. `ui open notes`. Create a note titled `bench H2` if the app offers a new-note row reachable with one guarded Enter (`keys send focus:<its row>| enter`, then `keys send text:bench H2`, then the app's save key as its hint line says). If creating a note needs anything outside the allowed rows, skip to step 3 and say so.
2. Expected after saving: the toast or status `Saved.`; `app caps` -> refused still R0. Log: a line showing the note was written to the SD card (quote; no note text).
3. Step to a row whose title starts with `bench`; `keys send focus:<that row>| enter`; use the app's delete key as its hint line says; `ui answer` -> `ALLOW`, label `delete`, title `Delete this note?`; `ui answer yes delete` -> `ui answer: confirmed 'delete'`. The row is gone (`ui`), status `Deleted.` If no `bench` note exists, report "not run".

## D. #274: a delete during a copy (Files)
1. Make a 600 KB file of zeros in `$S` named `wr-test.bin`; `tools/badge.sh ota-file "$IP" "$S/wr-test.bin" 'FLASH0:wr-test.bin'`.
2. `ui open files`; find `wr-test.bin` (one key per call); start its copy to the SD card with the keys the hint line names, guarded on `focus:wr-test.bin`. While the copy runs (within 2 s): `tools/badge.sh ota-rm "$IP" 'FLASH0:wr-test.bin'` -> expected an HTTP 409 with `couldn't delete ...: Permission denied` (quote, redacted). If the copy is too fast to hit, say so and report the reply you got.
3. After the copy: the file is still listed; then delete it and its SD copy through the Files delete (`ui answer yes delete` on `Delete wr-test.bin?`) or with `ota-rm` for the flash copy. Leave no `wr-test.bin` behind; say what remains if something does.

## E. #229: Timer
1. `ui open timer`; start a timer of the shortest length the app offers (guarded Enter on its own start control only); `ui open home`; wait for it to fire (at most 90 s).
2. `notify` -> a new entry from Timer (quote its title only); `app caps` -> refused still R0; `app life` -> the timer line (quote it).

## F. #264: one crash-log entry
1. `crash` -> if C0 is 0, report "not run: no entry to delete" and go to G.
2. `ui open crashlog` (or the name `ui open` lists for the Crash log); step to the OLDEST entry; `keys send focus:<its row>| enter` -> the full text is shown (do not paste it). Use the delete key the hint line names; `ui answer` -> `ALLOW`, `delete`, `Delete this crash entry? The core dump stays; Clear all erases it.`; `ui answer yes delete`.
3. `crash` -> the count is C0-1. `mem` -> record dma free and internal free.

## G. #286: Clear all notifications (the owner accepted the loss)
1. `notify` -> record the count N (do not paste entries). `ui open notifications` (or the name `ui open` lists); step to `focus: Clear all`; `keys send focus:Clear all| enter`.
2. `ui answer` -> `ALLOW`, label `clear all`, title `Clear all N notifications?` (quote with the number). `ui answer yes clear all` -> confirmed. The dialog closes; `notify` -> 0 entries (or only entries newer than the clear).

## H. Close
1. `ui open home`, `ui lock off`. `ui` -> stack `0`, `ui lock: off`.
2. `home` -> identical to `wr-home-before.txt`. `slot` -> `(valid)`; `crash` -> no new crash; `dev` -> on; `mesh` -> `identity: ok`, tx unchanged since A (record both).
3. `mem` -> dma free / min, internal free, psram free; compare with A2 (gate: dma free >= 89981, min >= 80650).

## Report (to the main session, under 450 words)
A table: step, pass / fail / not run, the one line or figure asked for. Then FINDINGS with the exact reply (redacted) and the file under `$S/` with the full output. Say plainly what was skipped and why. Do not interpret a failure as acceptable.
