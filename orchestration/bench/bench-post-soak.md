# Bench script: the flash after the baseline soak (main at the flashed commit; about 40 issues merged since fw-6b8febf7)

Source of order and decisions: the two comments on #407 of 2026-11-15 ("Plan ..." and "Owner decisions"). The baseline soak is over. One flash (C6 bundle, then P4), then the safety gate, ten restarts, Part A (agent, Wi-Fi), the engine comparison (main, its own script `bench-engine-compare.md`), Part B (USB console, owner present), Part C (owner's hands). File prefix for output: `wp-`. `<sha>` is the `fw-<sha>` label of the flashed commit (main fills it in).

Issues covered: #444 #446 #445 #447 #385 #233 #451 #453 #458 #454 #459 #456 #449 #455 #452 #448 #457 #461 #462 #443 #463 #464 #465 #466 #322 #467 #468 #469 #471 #472 #473 and the #260 pool counters.

Every command name, reply text or page name that no issue comment or source file confirmed is tagged **[CONFIRM]**: main checks it against `fw help all` on the new image BEFORE the agent starts, and fixes the line. Untagged lines come from an issue comment or the source tree.

Run order: F0a, F0b, F1-F3, S1-S4, R1, Part A, the engine script, B1-B4, C1-C7, then B5 together with C8, then B6, then B7 (optional).

## Pass lines (every part)
- No crash, no unplanned restart, no C6 restart (the planned ones are named in the steps), no heartbeat miss.
- **No task watchdog line in the serial log** (#471). The agent greps every log it saves for `task_wdt` and reports the count (expected 0); main confirms with the census of the USB serial log at the end (B6). The only allowed exceptions are the `fw storage` listings of F0b on the OLD image (they trip the watchdog there; cut a marker line in the serial log before and after them).
- DMA free >= 89981 and DMA min >= 80650 (the last soak: free 151091, min 142475).
- No line `CORRUPT HEAP`, `ui_check: FOUND`, no canary hit.

## Rules (all parts)
- On air: sends ONLY to the test channel (`#badgetest`), steps A2, B1, C4. No `dm`, `advert`, `trace`, `discover`, `telemetry`, `remote`, no `history <target>`, no zero-hop contact share (it transmits). A repeater login (C7) only on the owner's word.
- No `--wipe-storage`, no `sd format` with a real word, no `factory-reset ... YES`, no `setup`, no `noscreen`, no `stress`, no `thermal test`, no power-off. The erase lines in A5 use a WRONG confirm word on purpose: the expected reply is the origin refusal. A reply about the confirm word instead is a FINDING (the origin check is missing), not a pass.
- Since #455 a remote `fw sd format` and a remote factory reset are REFUSED; since #466 `fw c6 reset`, `reset rom`, `reattach`, `verify` and `reflash` are USB console only. Nothing here relies on any of them remotely; only the refusals are checked. Earlier scripts that formatted the card or reattached the C6 remotely (wave H step E, the #260 bench) must not be reused.
- **Gated forms go one per call, alone:** `psram` with an argument, `idle test`, `notify test`, `thermal` with an argument, `crash test`, `set disp.engine`, `dev off`, `noscreen`, `mesh ident simulate`. Never put one in the same shell command block as another line that reaches the badge.
- Never print or paste addresses, network or host names, MAC, token text, key material, coordinates, contact names, node names, message text or notification text. Raw output goes to `$S/wp-*.txt` (chmod 600); quote only the lines a step asks for. Pipe anything printed through `sed -E 's/([0-9]{1,3}\.){3}[0-9]{1,3}/<ip>/g'`. For #469 and #472 quote counts and byte sizes only.
- After ANY reboot: poll `ota-info` (sleep inside one foreground command, no `pgrep` loop), then wait 75 s.
- Stop and report if the badge is silent for more than 4 minutes, if `crash` shows a NEW crash that a step did not provoke, or if a "must stay refused" step is accepted.

## Tools (use exactly these)
- `S=<the session scratchpad named in the brief>`
- Console line over Wi-Fi: `"$S/fw.sh" "<line>" ["<line>" ...]` (lines without the `fw ` prefix; prints `>> line` and the redacted reply).
- `IP=$(cat "$S/badge-ip")`, then from /home/lucas/coding/why2025-badge: `tools/badge.sh ota-info "$IP"`, `tools/badge.sh log "$IP" > "$S/wp-log-N.txt"`, `tools/badge.sh crashes "$IP"`, `tools/badge.sh screenshot "$IP" "$S/wp-N.png"`, `tools/badge.sh ota-file "$IP" <file> <badge-path>`, `tools/badge.sh ota-rm "$IP" <badge-path>`.
- Wrong-token runs: prefix one command with `BADGE_OTA_TOKEN=00000000000000000000000000000000` (well-formed, wrong). Never touch the real token file.
- Before any key: `idle test 0` (gated: its own call), then `ui lock on 600` (renew on long steps). When done: `ui open home`, `ui lock off`. One key per call in lists; read `ui` for `focus:` after each; every Enter goes behind a guard `keys send focus:<Row>| enter`. Look at screenshots yourself; describe, do not attach.
- USB console and serial log (main only): `tools/badge.sh console` / `monitor` as in the `lichen-bench` skill; the serial logger and `tools/soak_report.py census` for the watchdog pass line.

## Allowed commands (the complete list; anything else is not allowed)
Wi-Fi console, agent and main (Part A, S, R): `slot`, `c6`, `crash`, `dev`, `mem`, `mesh`, `onair` (no argument), `help`, `help all` **[CONFIRM]**, `version` **[CONFIRM]**, `soak`, `boot`, `notify` (count only), `sd`, `storage` with `meshcore` or one other folder **[CONFIRM the folder names]** (only A9, F0b, A10), `get disp.`, `app caps`, `app life`, `app background` and `app background <id> on|off` (A13 only), `channels`, `mesh loc` (A3 only), `c6log <arg>` (A4 only), `mesh send #badgetest <text>` (A2 only), `factory-reset plan all`, `factory-reset all x`, `factory-reset settings x`, `sd format x` (A5 only, wrong word), `token new`, `dev on`, `onair off`, `c6 reset` (A5 only, expected refusal), `slot other` (A16 only, expected refusal), `ui`, `ui open <name>` **[CONFIRM names]**, `ui lock on|off`, `ui overflow`, `ui perf bench 10`, `ui answer` (read only), `ui answer yes delete` (only on "Delete wp-test.bin?"), `keys send ...` (Enter only on the rows named in A14, the copy key only in A8), `reboot` (R1, A12, A17 only).
Gated, one per call, main or agent (S3, A12, A17): `idle test 0`, `psram dma sweep 16384`, `psram dma 15`, `mesh hist scan 100`, `notify test info`, `crash test panic` (A17, owner's word), `crash clear` (A17, owner's word).
`badge.sh`: `ota-info`, `log`, `crashes`, `screenshot`, `ota-file`/`ota-rm` (only on the throwaway paths named in A6, A7, A16), `ota-c6`, `ota-firmware`, `ota-reboot` (main, flash order only).
USB console (main, Part B): `dev`, `dev off`, `dev on`, `channels`, `channel rm '#badgetest'`, `channel add ...`, `channel rm benchwp`, `mesh send #badgetest <text>`, `idle test 0`, `notify test info`, `mesh ident simulate` (dev OFF only, expected refused), `set disp.engine ppa` (dev OFF only, expected refused), `thermal`, `psram`, `sensors`, `stress status`, `help all`, `c6 reset`, `c6`, `token new`, `token` (never pasted), `reboot`.

## Flash order (main session)
- **F0a** (before the flash, OLD image, owner at the badge, 30 s): Settings > Backup > Save to the card, so a backup in the OLD format (no end line) exists. Serves #453 (C3).
- **F0b Before-values on the OLD image** (#407 plan step 1; main, remote; write to `wp-before-*.txt`):
  1. Open a serial-log marker; start the USB serial logger (it runs to the end of the whole bench).
  2. `version` **[CONFIRM]**, `slot`, `crash` (record the entry count C0), `notify` (record the count N0), `dev`.
  3. `mem`: dma free/min, internal free/min, psram free, largest free blocks. `soak`: record `mslow`, `notice_slow_ms` (soak key `nslowms`) **[CONFIRM the key name]**, `storage_count_ms`, and every key it prints.
  4. `get disp.` -> expected `disp.pclk` 34285714, `disp.refresh` 446.
  5. `mesh`: rx, tx, failed, contact count K0 (count only) and, if the command exists, the favourites count **[CONFIRM: no favourites command found in an issue]**.
  6. `c6`: the pool lines `c6: pool tx depth 10 ... peak .. waits 0 drops 0 bad_frees 0 lost 0 no_pool 0` and the rx line. Record peaks (idle soak: tx 2 of 10, rx 4 of 24).
  7. `sd`: record the line (mounted, free, errors). Then `storage` and `storage meshcore` (ONCE each; they trip the task watchdog on this old image, #471, as a warning only): record free `/storage` bytes, the size of `bl/`, the node count. Close the marker.
  8. `ui perf bench 10` on home, MeshCore Messages, MeshCore Contacts (median of three) and on the Bench app (twice) **[CONFIRM page names]**; the old figures to beat or match: redraw 66 ms, flush 2.9 ms per strip, scroll 14.6 fps. One screenshot of home.
  9. Save `fw help all` as the copy for B3 **[CONFIRM the command]**.
- **F1** C6 bundle FIRST (#452 changes the C6 image): list off; `tools/badge.sh ota-c6 "$IP"`; wait for the C6 to come back; `c6` -> `IDENT match`, `up`. Quote it. Straight after the upload returns, `c6` once more and record the pool lines (#260: the pool peak under a real upload; the counters may reset when the C6 link restarts: say which).
- **F2** P4 image: `fw c6` once before the push (baseline), `tools/badge.sh ota-firmware "$IP"`, and if the badge still answers after the upload and before it reboots, read `c6` again (the pool peak of a real P4 upload) **[CONFIRM whether the badge reboots by itself at the end of the upload; if so this read is impossible and A7 supplies the load figure]**. Then poll `ota-info` and wait 75 s.
- **F3** `tools/badge.sh ota-info "$IP"` -> `badge_fw fw-<sha>`; save `wp-log-firstboot.txt`. `slot` -> running `(valid)`.
- **F3b** (optional, owner's word): to test a power cut during the first card write, the owner presses the hard reset about 60-100 s after the F3 boot, before anything else (the first start is the only one with an empty card folder). Evidence: the next start loads and completes (`adverts loaded`, `contact list on the card`). Without this step the cut is not benched (a console `reboot` is a clean stop, which saves).

## Safety gate (S1-S4, main, remote; stop on any failure)
**S1. First boot (#469, #472, #467, #446).** Read `wp-log-firstboot.txt`. Quote: two frame buffers of 1012 KB and "direct flush (CPU), 2 buffers", no `swap: no end of frame` at idle (#467); `adverts taken over from the per-node files` and, later, `the per-node advert files are gone` (#469); `card: contacts.bin none ... plan 2` then, within about 2 minutes, `contact list on the card: N contacts` (#472); the developer build's journal self-test line (#446); no `journal write of ... failed`; no notice that `lora.toml` was out of range (#452); `task_wdt` count 0. `crash` -> this boot clean; `crash` entry count equals C0 and `notify` count equals N0 (#463: the upgrade path keeps the crash records and notices).
**S2. Display (#322, #467, #468).** `get disp.` -> `disp.pclk` 48000000, `disp.refresh` 625. `get disp.engine` -> `cpu` (#468: every boot starts on `cpu`). `mem` against F0b: PSRAM free about **2.7 MB** lower (the frame buffer 1 036 800 B, the advert table 1 056 000 B, the contact table about 565 000 B plus about 35 000 B scratch); internal about **4 KB** lower (statics of #467 112 B, #468 2030 B, #472 about 1.7 KB, #464 164 B, #461 and #452 96 B each); DMA free and min within about 120 B of F0b.
**S3. PSRAM/DMA gate (#322, #467).** Each in its own call: `psram dma sweep 16384` three times, `psram dma 15` seven times, `mesh hist scan 100`. Expected: `0 bad bytes`, no flips, SD legs `direct 0, bounced 0 bad bytes`. Any flip, `CORRUPT HEAP` or canary: stop (fall back to 40 MHz or one buffer before going on; main decides).
**S4. Pool and links.** `c6` -> `IDENT match`, `up`, restarts 0; the pool lines `mem psram`, `lost 0`, `bad_frees 0`, `no_pool 0`.

## R1. Ten restarts (#322 display start, #469, #472; main, remote)
Ten times: `reboot`, poll `ota-info`, wait 75 s, then `slot`, `crash`, `mesh`, `get disp.`. Expected each time: slot unchanged, no crash, `get disp.` 48000000 / 625, `adverts loaded (snapshot N ...)` with the same count and, with the card in, `the card holds the contact list now: N contacts read from it (0 new here ...)` with the same N and plan 1 (#472), the frame-buffer lines present, `task_wdt` 0. A reboot is a clean stop, so the snapshots are saved at each stop (#469, #472).

## Part A. Remote (an agent runs it over Wi-Fi; list OFF, developer mode ON, except A16)

**A1. Health - #452, #455, #446, #471, #473, #469, #472.**
`ota-info`; `slot`; `c6`; `crash`; `dev`; `mem`; `mesh`; `onair`; `app caps`; `soak`; `help`.
Expected: `badge_fw fw-<sha>`; slot `(valid)`; c6 `IDENT match`, `up`, restarts 0; `crash` this boot clean; `dev` on; `mem` per the pass lines; `mesh` -> `identity: ok`, record rx, tx, failed, contacts, and quote the shape of the `adverts:` line (count, refused, snapshot generation, last write place/bytes/ms, written, failed) and the `contacts:` line (place, cap, card file number, last write, errors, what `/storage` keeps, merge counts), numbers only; `onair` -> list off; `app caps` -> `0 refused since boot`; `soak` -> record `mslow` (0), `wkmax` (#471, ms, expected at or under 100), `nslowms`/`notice_slow_ms` (#473: record against the old 4144 ms; the count runs every 10 minutes, so it is only valid 10 minutes after boot); `help` -> record `N of M commands registered` (the soak notes 60 of 96 after #464).

**A2. Test-channel send and reception - #452, #451, #448, #462.**
`mesh` (note tx T0), `mesh send #badgetest "bench post-soak 1"`, `mesh` after 20 s. Expected: tx = T0+1, failed unchanged, the peer session reports the message heard on `#badgetest` (main asks it; "heard" is the evidence). Read `log` and quote the count of repeater status-poll reply lines and that no line says a frame was dropped as too long (#448, #462; if the poll has not run, re-check in A18).

**A3. Position stays out of the log - #449.**
`mesh loc` (save the reply to `$S/wp-loc.txt`, chmod 600, DO NOT print it), then `tools/badge.sh log "$IP" > "$S/wp-log-3.txt"`. Expected: the asker got the full reply; in `wp-log-3.txt` the count of lines holding `loc` with a decimal number such as `12.3456` is 0. Quote the count only. `set bridge.secret` redaction needs an on-air remote CLI line: NOT run (covered by `mesh_rset_test`).

**A4. c6log argument - #458.**
`c6log 10s` -> refused (quote it). `c6log 0` -> refused. `c6log 10` -> runs (quote the first reply line). `c6` -> still `up`, restarts unchanged.

**A5. Refusals over Wi-Fi - #455, #464, #466, #456.** Expected each: `refused: only from the USB console` (#464 and #466 comments), the state unchanged. Run one per call:
`factory-reset plan all` -> still answers (the plan). `factory-reset all x`, `factory-reset settings x`, `sd format x` -> the USB-only refusal (quote). `dev on`, `token new`, `onair off` -> the same refusal. `c6 reset` -> the same refusal and the C6 restart counter in `c6` does NOT step (#466; the other four C6 forms are pinned by the host test and not tried remotely: a `reflash` that was accepted would rewrite the C6). A reply about a confirm word instead of the origin is a FINDING. Then `onair` still off, `dev` still on, `ota-info` with the same token still works (#456), `slot`/`crash` unchanged.

**A6. Token gate on every route, without touching firmware routes - #447, #456, #454, #459.**
For each of `ota-info`, `log`, `crashes`, `screenshot`, a console line (`tools/badge.sh fw "$IP" help`), and `ota-rm "$IP" 'FLASH0:wp-none.bin'`: once with the WRONG token prefix (each answers 401; quote the status), once with the right token (each answers as before; a missing file answers not-found). A wrong method: `printf 'header = "Authorization: Bearer %s"\n' "$(tr -d '[:space:]' < ~/.config/why2025-badge/ota-token)" | curl --config - -s -o /dev/null -w '%{http_code}\n' -X POST "http://$IP:8080/info"` -> 405 (the code only). An unknown path 404 and a malformed head 400 are pinned by `ota_route_test`; they answer before the token check by an older, separately filed finding: do not call that a regression. Firmware, C6 and piece routes are NOT probed without a token (a bug there would flash); F1 and F2 showed they work with it.

**A7. File push, delete and the pool load - #447, #454 (list off), #260.**
Make a 600 KB zero file `$S/wp-test.bin`. `ota-file "$IP" "$S/wp-test.bin" 'FLASH0:wp-test.bin'` -> 200. Immediately `c6` -> the pool lines: record `peak`, `waits`, `drops`, `bad_frees`, `lost`, `no_pool` (#260: the load figure; expected `waits 0 drops 0 lost 0 bad_frees 0 no_pool 0`; a non-zero `drops` is a finding for the depth tuning, the first reattach once dropped 20). KEEP the file for A8; the delete is at the end of A8.

**A8. Files copy to the card, and a delete during it - #443, #465, #274.**
1. Read the failing file's name from the issue? It is not on the issue and the card was formatted since: if the original is gone, use `wp-test.bin`; say so.
2. `ui lock on 600`; `ui open files`; step (one key per call) to Internal storage and `focus:wp-test.bin`; start the copy with the copy key the hint line names, guarded `keys send focus:wp-test.bin| c` **[CONFIRM the key]**. Within 2 s run `tools/badge.sh ota-rm "$IP" 'FLASH0:wp-test.bin'` -> HTTP 409 (`couldn't delete ...: Permission denied`; quote, redacted); if the copy is too fast to hit, say so.
3. Expected: the copy completes and the card file has the right size (read it in the Files screen of the card; screenshot), or the log line `files: copy: step=<sd_begin|src_stat|src_open|dst_open> errno=<n>` names the failing step (#443, #465: the step names `src_read` and `dst_write` exist too) and the status text names the cause. Quote the line or "copy ok".
4. A name with `?` (#443): `ota-file` a file named with a `?` to `FLASH0:`; if the tool or the path rule refuses the name, say so (the pure rule `files_name_ok_fat` is host-tested); if it lists, the copy key must give the name message at once **[CONFIRM the path rule accepts the character]**.
5. Clean up: delete `wp-test.bin` and its card copy (`ui answer yes delete` ONLY on "Delete wp-test.bin?", or `ota-rm` for the flash copy). Leave nothing behind; say what remains.

**A9. Storage listings, once each - #471.** The #471 bench is exactly this: the listings that tripped the watchdog on the old image, now with the census read after them. Before: `soak` -> record `wkmax`. Then `storage`, `storage meshcore`, and one deep folder **[CONFIRM: a large folder on the new image; the 360-file `meshcore/bl` of the soak is already being removed by the #469 takeover]** (ONCE each, never in a loop). If `bl/` still lists, record the entry count. After: `soak` -> `wkmax` at or under 100 (record it); `tools/badge.sh log "$IP" > "$S/wp-log-9.txt"` -> `task_wdt` count 0. A non-zero count is a FINDING on #471.

**A10. Advert table and its takeover - #469.** No contact names or key material anywhere.
1. `mesh` -> the `adverts:` line shows `1 written` and the place `on the card` (after the forced first snapshot, 60 s after the first start); `failed 0`.
2. `storage meshcore`: no `bl/`, no `adverts.bin` (the card is in); free `/storage` up against F0b by the old `bl/` blocks (record both numbers). On the card: `lichen/meshcore/adverts.bin` exists **[CONFIRM how to list the card: the Files app on the card, screenshot]**.
3. The contact count equals K0. Export (NOT share): a contact heard before the flash exports **[CONFIRM the export command; the zero-hop share transmits and is out of scope]**; if there is no remote export, this is hand step C5.
4. Boot-log lines of S1 and R1 cover the takeover and the load after a restart.
5. `soak`: free `/storage` over the run no longer steps down 8192 B per few new nodes (read at A18 against F0b).

**A11. Contacts on the card - #472.** Counts and sizes only.
1. `mesh` -> the `contacts:` line says `list on the card, room for 4000`; contact count equals K0; favourites unchanged (same command as F0b).
2. Log: `contact list on the card: N contacts`; the next `contacts saved after the unlock` line ends in `(K on /storage)` with K at most 250 plus the always-in group (favourites, conversations, repeaters and rooms with a saved password); if no such line appears during the run, say so (the internal file is written on an owner change, else every 6 h and at a stop; a `reboot` in A12 triggers it).
3. `storage`: `contacts3` shrunk against F0b (record both sizes).
4. Save times against #425: no `MeshCore loop took` line for a contact save; the card write's `ms` from the log; `soak` -> `mslow` 0 (1 h later is not required; read it at A18).
5. `mem` before/after the first card write: record the PSRAM dip (up to about 2 x 610 KB for the seconds of the write).

**A12. Persistence through the new replace helper - #463, #459, #456.**
`crash` (count must equal C0 from F0b and S1); `notify` (count N0). `notify test info` (gated, own call) -> a test notice appears; `notify` count N0+1; `reboot`; poll, 75 s; `notify` -> N0+1 (a test notice survives a reboot); `crash` count unchanged; `ota-info` with the token works (the token file written by the helper authorises after a reboot); `ota-file` of a 4 KB file then `ota-rm` it (an update path that needs the token) -> 200 / 200.

**A13. Background list round trip - #461, #233.**
`app background` -> save the list to `wp-bg-before.txt`; pick ONE app id that is not MeshCore (MeshCore is "Always on"). `app background <id> on` then `app background` -> removed as the issue describes (quote the one line). Restore with `app background <id> off` and `app background` -> identical to `wp-bg-before.txt` (`diff`). If it does not restore, STOP and report. **[CONFIRM the on/off meaning against `fw help all`]**.

**A14. One UI pass - #444, #233.** (one `ui lock` session; Enter ONLY on `Apps` and one app row at a time; the owner's word is needed for this Enter)
1. Screens using the shared label helper (#444): `ui open home`, `about`, `timer`, the MeshCore contact list and grid screen **[CONFIRM names]**. One screenshot each; describe: text present, not clipped, status-bar clock ticking (two screenshots 60 s apart on home).
2. Settings > Apps (#233): `ui open settings`, `focus: Apps`, `keys send focus:Apps| enter`; one screenshot. For each app row: `keys send focus:<app>| enter`, wait 3 s, screenshot, `ui overflow`, `esc`. Expected per page: capabilities note, Data (badge storage and SD card), Background (MeshCore "Always on"), Crashes, a Clear data row for every app except MeshCore and the OS apps. NEVER Enter on `Clear data`, a Background row, or anything inside MeshCore.
3. The Background row of the app picked in A13 reads the same as `app background` (quote both).
4. `ui open home`, `ui lock off`.

**A15. Overflow, stack margin, memory - #233.**
`ui overflow` -> 0. `log` -> `wp-log-15.txt`; quote every `stack margin` line (the job's 6144 B stack after a measure). `mem` -> compare with A1.

**A16. On-air hold on files, and `slot other` - #454, #464. NEEDS THE LIST ON.** Main asks the owner to run the on-air helper `sync`; the owner runs `off` after. Throwaway names only, never the real `lora.toml`, the MeshCore identity or the token file.
1. `onair` -> list on.
2. `ota-file "$IP" "$S/wp-small.bin" 'FLASH0:lora-wp.tmp'` -> 409 with the hold text (quote). The same for `'FLASH0:ota_wp.tmp'` -> 409. `ota-rm "$IP" 'FLASH0:[meshcore]wp-none.tmp'` -> 409. Control: `'FLASH0:wp-test.bin'` -> 200, then `ota-rm` it -> 200.
3. `slot other` over Wi-Fi -> refused while the list is on (#464). `slot` afterwards: if it changed (the line was accepted), run `slot other` again after step 4 to revert and report a FINDING. Never `reboot` between.
4. Owner runs `off`; `onair` -> list off. With the list off: `'FLASH0:lora-wp.tmp'` -> 200, `ota-rm` -> 200; the same for `'FLASH0:ota_wp.tmp'`. Leave nothing behind.

**A17. Crash record - #463. OWNER'S WORD (this erases the crash log and restarts the badge once).**
Before: `tools/badge.sh crashes "$IP" > "$S/wp-crashes-before.txt"` (keep it). `crash test panic` (gated, own call; a developer-only test arm; the one planned crash) -> the badge restarts; poll, 75 s; `crash` -> exactly one new entry, listed (this is the one crash the stop rule allows); `crashes` lists the earlier records plus the new one. Then `crash clear` (own call) -> `crash` reads 0. State plainly that the owner's earlier records are gone after this step (the file from the first command is the copy).

**A18. Close - #448, #473, #469, #260, #471.**
`ui` -> stack 0, `ui lock: off`. `slot` `(valid)`; `c6` `IDENT match`, restarts unchanged except what steps provoked, the pool lines (record the final peak, waits 0 drops 0 lost 0 bad_frees 0 no_pool 0); `crash` (0 after A17, else C0); `dev` on; `onair` off; `mesh` -> `identity: ok`, tx = T0+1 plus nothing else, failed unchanged; status-poll replies seen (#448: the count); `mem` against F0b and A1; `soak`: `mslow` 0, `wkmax` at or under 100, `nslowms`/`notice_slow_ms` (#473: record; the old 4144 ms; the target for the follow-up is under 500, so if it is still seconds the follow-up decision on #473 goes to the owner), free `/storage` trend; every saved `wp-log-*.txt` grepped: `task_wdt` count (expected 0) and `CORRUPT HEAP` (0).

## Engine comparison (main, remote; its own script)
Run `orchestration/bench/bench-engine-compare.md` here: developer mode on, keyboard locked, each engine `cpu`, `ppa`, `ppa-async`, the owner's hands slice C1 interleaved. Covers #468, #467, #322; it decides the default engine, whether the PPA engines stay in the image, a third frame buffer, and 62.45 Hz against 60.0 Hz.

## Part B. USB console (main session, owner present; Wi-Fi refuses these by design)

**B1. Channel add rules and key entry - #451, #445.**
`channels` -> save the names list (names only) as `before`. With the owner's ok (it removes the test channel for about a minute):
1. `channel rm '#badgetest'`.
2. `channel add '#badgetest' <the well-known Public channel key, as in host_tests/mesh_share_test.c>` -> REFUSED with the "needs that hashtag's key" text (quote). `channels` -> no `#badgetest`.
3. `channel add '#badgetest'` (no key) -> added. `channels` -> the same names as `before`; record whether the order changed.
4. `mesh send #badgetest "bench post-soak 2"` -> tx +1, the peer hears it.
5. Key entry (#445, hex decode): `channel add benchwp 00112233445566778899aabbccddeeff` -> added; run the share command `help` names, if any, and quote only the SHAPE of the link (no key); then `channel rm benchwp`. `channels` -> same as `before`.
6. `onair` (the list show, #445) -> the same status as A1.
A foreign-keyed stored `#name` marked in `channels` cannot be created on a clean badge now: host-tested only.

**B2. Developer-mode-off sweep - #421, #464, #455, #468.** (`dev` must read on at the end)
`dev off`. Each of these, one per call, must answer `developer mode is off` (#464 comment; quote one reply, the rest "same"): `idle test 0`, `notify test info`, `psram dma 15`, `mesh ident simulate` (#455: if it is NOT refused it blocks LoRa transmit until restart: `reboot`, wait 75 s, report a FINDING), `set disp.engine ppa` (#468: refused; `get disp.engine` still `cpu`). The plain forms still answer: `thermal`, `psram`, `sensors`, `stress status`, `mesh`. `dev on`; `dev` -> on; `idle test 0` -> accepted.

**B3. Full help against the saved copy - #464, #423.** `help all` **[CONFIRM]** -> equals the copy saved in F0b except for the rows the issues added or changed (list the differing rows by name only: the refusal texts of #464 changed; the C6 forms now UART-only; `disp.engine` registry key). `N of 96 commands registered` (60 of 96 expected after #464); rule tables 24 of 40 in the developer image.

**B4. C6 forms from USB - #466.** `c6` -> record the restart counter R. `c6 reset` -> the C6 resets as before; poll `c6` -> `IDENT match`, `up`, restarts R+1; keyboard, LoRa and Wi-Fi recover (a remote `ota-info` answers; one test-channel send is NOT needed). The refusal over Wi-Fi was checked in A5.

**B5. Token rotation, USB half - #456, #463. LAST of the remote work, run together with C8.**
Owner first copies the host token file aside as the OLD token (do not print it). `token new` -> a secret line shown only on the owner's screen (main never pastes it) plus the text that it takes effect at the next restart. `token` -> says the running server keeps the old token until restart. `ota-info` with the old token still works now (quote the status). (#463: the new token goes through the atomic replace helper; #459: the stack copies are wiped.) Then C8.

**B6. Serial-log census - #471 pass line, all parts.** After C8: `tools/soak_report.py census` on the USB serial log of this bench -> `task watchdog triggers in the serial log: N (pass: 0)`; subtract nothing except the F0b old-image listings (the marker lines) and the A17 provoked crash window. Quote the line and the rate of every log kind that rose (the census's usual view).

**B7. Optional, only with the owner's ok - #385.** After C8 (token settled). On a scratch branch: `tools/badge.sh new-app demo "Demo"`, the full build, OTA the scratch image, `ui open home` screenshot shows the Demo tile; drop the branch and re-flash `fw-<sha>` (a second OTA and reboot; the one deviation from the single flash, so last).

## Part C. Hands (the owner at the badge; the main session reads the console)
Everything here is a physical key press. No remote Enter on any of it. The must-do list is C1, C2, C3, C8 (about 15 minutes); C4 to C7 are about 15 more and the owner picks.

| Step | Issue | Minutes | Needs |
|---|---|---|---|
| C1 | #322 #467 #468 | 6 | interleaved with the engine script |
| C2 | #233 | 2 | a test app |
| C3 | #453 | 3 | F0a done |
| C4 | #451 | 1 | on air, test channel |
| C5 | #472 #469 | 8 | the card |
| C6 | #472 | optional | owner's word, a PC |
| C7 | #462 | 1 | owner's word, on air |
| C8 | #456 | 2 | after B5 |

**C1. Picture, once per engine - #322, #467, #468 (6 min, with the engine script).** For each of `cpu`, `ppa`, `ppa-async` (main switches the engine): scroll Contacts for 10 s by hand and look at the picture at 10 % backlight (#254): calm, colours and position as before, no tearing while scrolling, no flicker between old and new content, no stale strips. Report per engine "calm / flicker / tearing" and the screen. If it is not calm: note the engine and screen; the fix order is front porch 20 -> 31 (60.0 Hz) first, then 40 MHz.

**C2. Clear data on a test app - #233 (2 min).** Settings > Apps > a small non-OS app with data > Clear data. Expected: the careful dialog opens; the app's settings and its two directories are cleared; a restart hint if the app ran; its Data row reads 0. Main: `ui answer` while open -> refused for a remote answer (quote the line), the dialog stays until the owner answers.

**C3. Settings backup, old and new format - #453 (3 min).** (F0a made an old-format backup.) Settings > Backup: Load -> the summary of the old backup shows (a file without an end line still loads); do not apply; cancel. Save to the card -> `Saved`; Load -> the same summary; cancel. Main: `log` has no SD write error.

**C4. A chat send from the UI - #451 (1 min).** Messages > `#badgetest` > one short message. Expected: it sends, the peer hears it. Main: `mem` and the task-stack lines right after: the LVGL stack watermark has not dropped below the margin recorded in A15 (quote the figure).

**C5. The card in, out, and absent - #472, #469 (8 min).** Each with the console watching:
1. Pull the card while running: log `the card is gone`, `room for 1000`, a save with everything on `/storage`. Put it back: the merge, then `list on the card`; contact count and favourites unchanged.
2. Start without the card: the core set is there, MeshCore works; put the card in: the rest comes back; the advert table starts from `/storage` or empty and refills from the card's snapshot at the next start.
3. Delete one contact with the card out, put the card in: it stays deleted (`contacts.del`).
4. Export or share of a contact heard before the flash works after a restart (the table loaded from the snapshot).
5. Not planned: filling the table to 4000.

**C6. A card file of another key - #472 (optional, owner's word, needs a PC).** Copy a contact file of another badge onto the card, or change one byte in the head's check. Expected: the notice, `list on /storage (the card's file is left alone)`, the file unchanged; then MeshCore settings > Contacts on the card > confirm: `contacts.old` on the card, a new list. Never print the contents.

**C7. Repeater login - #462, #288 (1 min, ON AIR to the agreed repeater, ONLY with the owner's word).** One login with the saved password. Expected: it succeeds (a real login reply is 13 bytes). If declined, "not run".

**C8. Token handover - #456 (2 min; after B5).** The owner puts the NEW token in the host token file. Main: `reboot` at USB; wait 75 s. `tools/badge.sh ota-info "$IP"` with the new token works; the same call with the OLD token (set by the owner, not pasted) answers 401; the serial log tool hides the new token's line (quote "hidden"). `crash` clean. From here the remote part is closed.

## Contradictions and ordering conflicts between the issues
1. **#452**: the body says a 5 s airtime ceiling; the later decision keeps 8 s. This script follows the decision. The bad-preamble `lora.toml` test is NOT scripted (owner's word).
2. **#455 vs earlier scripts**: wave H and the #368 proof used a remote SD format; refused now. Only the refusal is checked (A5).
3. **#454 vs the standing state**: the hold test needs the list ON, which refuses Wi-Fi updates; A16 runs after all flashing and the owner switches it back off.
4. **#456 vs everything remote**: the token rotation cuts remote access until the host copy is updated; it is the last remote action (B5/C8).
5. **#447**: the issue says the token check is the single gate for every route; the later comment records that 404/405/400 answer before it. A6 treats only a 200 without a token as a failure.
6. **#385 vs one flash**: the demo-app step needs a second image; optional and last (B7).
7. **#233 vs the wave-H2 Enter rules**: those forbade Enter on Settings rows; A14 needs Enter on `Apps` and one app row at a time (owner's word).
8. **#461 vs #233**: both touch `app background`; A13 changes the saved list and restores it.
9. **#260 vs #466**: the #260 bench reads the pool after `fw c6 reattach` three times; since #466 reattach is USB only. The reattach evidence is not repeated remotely; the pool counters are read after the uploads (F1, F2, A7) and at A18. If a reattach proof is wanted, main does it at USB (a B4 extension).
10. **#471 vs #469/#472 benches**: #469 wants a before-listing of `fw storage` on the old image, and #471 says exactly those listings tripped the watchdog there (a warning). F0b takes the listings once on the old image (marked in the serial log, excluded from the census). #471's own bench wants the 360-file walk, but the takeover of #469 starts removing `bl/` 60 s after the first start, before the 75 s console wait ends, so on the new image the 360-file case is likely gone; A9 records what is left and does not claim the old case.
11. **The #407 plan vs #469/#472**: the plan expects PSRAM "about 1 MB lower" and internal "about 2 KB lower"; #469 (table always 4000 x 264 B = 1 056 000 B) and #472 (about 565 KB table growth plus 35 KB scratch) make it about 2.7 MB and about 4 KB. S2 uses the new figures.
12. **#469 body and decision comment vs the build**: the decision sized the table at about 600 KB with a card and 1000 without; the build is always 4000 slots (1 056 000 B), as its merge comment admits. #472's table (about 753 KB) is a second permanent table. S2 and A11 record both.
13. **#322**: the decision comment says refresh 624, the merge comment corrects it to 625. This script uses 625.
14. **#468 vs the engine run**: its bench requires developer mode OFF for the refusal check, the comparison requires it ON. The refusal is in B2, the comparison before it.
15. **#463 vs the stop rule**: A17 provokes a crash on purpose. The stop rule and the census name it as the one allowed crash.
16. **#443**: the bench wants "the failing file's name first", but that file is not named and the card was formatted since; A8 falls back to `wp-test.bin`.
17. **#464 vs #455/#456/#466**: consistent (all USB-only forms answer the same refusal text); the only addition is that `dev on`, `onair off` and `token new` are also tested remotely in A5.

## Steps that need the owner's word
A14 (Enter on `Apps` and app rows); A16 (the owner runs the on-air helper `sync` and `off`); A17 (`crash test panic` and `crash clear` erase the crash log and restart once); F3b (a hard cut at first boot); B1 (removing the test channel for about a minute); B7 (a second throwaway image); C6 (a card file of another key); C7 (the repeater login on air); and every hands step C1 to C5 and C8 (the owner is the one doing them). Not scripted without the owner's word: the bad-preamble `lora.toml` test (#452), the `set bridge.secret` remote line (#449), `ppa-async` is run with the owner at the badge.

## Table: issue, step(s), what closes it
| Issue | Step(s) | Closes when |
|---|---|---|
| #444 | A14.1 | screenshots of home, clock, About, Timer, MeshCore contacts and grid look as before |
| #446 | A1, S1 | boots clean; the dev build still logs the journal self-test |
| #445 | B1.5, B1.6, S1 | console key entry, `onair` show and the share link shape behave as before |
| #447 | F1, F2, A6, A7 | info, log, screenshot, console, file push/delete, firmware update and C6 push work with the token; the non-flashing routes answer 401 without it; wrong method 405 |
| #385 | B7 (optional) | the Demo tile shows after the update; branch dropped, `fw-<sha>` restored |
| #233 | A13, A14, A15, C2 | a page per app, `ui overflow` 0, a `stack margin` line, Background row equals `app background`, one Clear data works by hand and refuses remote |
| #451 | B1.1-4, C4 | foreign-key add refused, restored channel sends, UI chat send stack margin ok |
| #453 | F0a, C3 | old-format backup loads, a new Save then Load shows the same summary |
| #458 | A4 | `c6log 10s` refused, `c6log 10` runs |
| #454 | A16 | list on: `lora`, `ota_` and MeshCore-dir paths get 409; list off: 200; control path 200 |
| #459 | no step of its own | covered by A6, A12 and B5 (the token path works after the change) |
| #456 | A5, B5, C8 | remote `token new` refused; USB rotation: new token works, old gets 401, secret hidden in the log |
| #449 | A3 | no position line in the log after `mesh loc`; the `set bridge.secret` check NOT run |
| #455 | A5, B2 | `factory-reset` and `sd format` refused over Wi-Fi, list off; `mesh ident simulate` refused with dev off |
| #452 | F1, A1, A2, A18, S1 | both chips on new images, link up, a test-channel send heard, no fallback notice |
| #448 | A2, A18 | no step of its own: closes when status-poll replies still arrive |
| #457 | no step of its own | covered by the Wi-Fi join at F2 and A1 |
| #461 | A13, S1 | `app background <id> on` then the list shows it removed; boot clean |
| #462 | A2, C7 | no remote step of its own; closes with a successful repeater login (C7); without the owner's word it stays open |
| #443 | A8 | the copy completes with the right size, or the log names step and errno; a `?` name gives the name message; a delete during the copy answers 409 |
| #463 | S1, A12, A17, B5 | crash log and notices survive the upgrade; a test notice survives a reboot; one test crash is recorded and `crash clear` empties it; the token authorises after a reboot and a rotation |
| #464 | A5, A16.3, B2, B3 | the USB-only forms refused over Wi-Fi; `slot other` refused with the list on; dev-off sweep; `help all` equals the saved copy plus named differences; one test-channel send (A2) |
| #465 | A8 | no step of its own: closes with A8 (the step names show in the log if a copy fails) |
| #466 | A5, B4 | `c6 reset` over Wi-Fi refused and the C6 counter does not step; from USB it resets as before |
| #322 | S2, S3, R1, C1 | `disp.pclk` 48000000, `disp.refresh` 625, PSRAM gate clean, picture calm at 10 % backlight; else front porch 31 |
| #467 | S1, S2, S3, E, C1 | two frame buffers in the log, PSRAM free about 1 MB lower for it, no flips, `flip:` 0 timeouts, no tearing; wait and copy figures recorded for the third-buffer decision |
| #468 | E, B2, C1 | per-engine figures recorded, no fallback line, no stale strip, dev-off refusal; the owner decides the default and whether the engines stay |
| #469 | S1, R1, A10, C5 | takeover lines, `adverts:` line `1 written` on the card, `bl/` gone, free `/storage` up, count unchanged, load after restart, no 8192 B steps in the soak trend |
| #471 | A1, A9, B6 | the three listings with no `task_wdt` line, `wkmax` at or under 100, census prints 0 |
| #472 | S1, R1, A11, C5, C6 | list on the card with room for 4000, counts and favourites unchanged, `(K on /storage)` K at most 250 plus the always-in group, save times within #425's figures, card pulled/absent/foreign cases |
| #473 | A1, A18 | not built: only the measured `notice_slow_ms` after the flash against 4144 ms (target under 500); if still seconds, the owner picks an option |
| #260 | F1, F2, A7, A18 | pool peak and `waits/drops/lost/bad_frees/no_pool` under a real upload recorded, all zero except a peak; the reattach proof is not repeated remotely (contradiction 9) |
