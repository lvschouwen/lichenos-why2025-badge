# Bench script: the single flash after the baseline soak (19 issues merged on main, not yet flashed)

**DRAFT (2026-11-14), compiled by a prep agent from the issue comments, not yet run.** Before use, main confirms against `fw help all` on the new image: the `mesh loc` and share command names, the `onair` show form, the USB-only refusal text, the 401/405 status codes and the `ui open` page names. Owner's word needed for: Enter on Settings > Apps rows (#233, A10), the repeater login (C4, the only step that covers #462), the bad-preamble `lora.toml` test (#452, not scripted). Add #443, #463, #464 steps when they merge.

Run only AFTER the soak has ended (2026-11-16 03:46 UTC): a power-off or reboot restarts its clock. One flash (C6 bundle, then P4) and one grouped bench. Issues: #444 #446 #445 #447 #385 #233 #451 #453 #458 #454 #459 #456 #449 #455 #452 #448 #457 #461 #462. File prefix for output: `wp-`. `<sha>` is `fw-<sha>` of the main commit that is flashed (the main session fills it in).

Run order: F0 (before the flash), F1-F4, Part A, B1-B2, Part C (C1-C4), then B3 together with C5 (the token rotation cuts remote access until the host copy is updated), then B4 (optional).

## Rules (all parts)
- On air: sends ONLY to `#badgetest` (A3, B1, C3). No `dm`, `advert`, `trace`, `discover`, `telemetry`, `remote`, no `history <target>`. A repeater login (C4) only on the owner's word.
- No `--wipe-storage`, no `sd format <real word>`, no `factory-reset ... YES`, no `setup`, no `noscreen`, no `stress`, no `thermal test`, no power-off. The commands that look destructive in steps A6 and A7 use a WRONG confirm word on purpose: the expected reply is the origin refusal, and a reply about the confirm word instead is a FINDING (the origin check is missing), not a pass.
- Since #455 a remote `fw sd format` and a remote factory reset are REFUSED. Nothing in this script relies on them remotely; only the refusal is checked. Earlier scripts that formatted the card remotely (wave H step E) no longer work and must not be reused.
- Never print or paste addresses, network or host names, MAC, token text, key material, coordinates (the owner's saved position), contact names, message text or notification text. Raw output goes to `$S/wp-*.txt` (chmod 600); quote only the lines a step asks for. Pipe anything printed through `sed -E 's/([0-9]{1,3}\.){3}[0-9]{1,3}/<ip>/g'`.
- After ANY reboot: poll `ota-info` (sleep inside one foreground command, no `pgrep` loop), then wait 75 s.
- Stop and report if the badge is silent for more than 4 minutes, if `crash` shows a NEW crash for this image, or if a "must stay refused" step is accepted.

## Tools (use exactly these)
- `S=<the session scratchpad named in the brief>`
- Console line over Wi-Fi: `"$S/fw.sh" "<line>" ["<line>" ...]` (lines without the `fw ` prefix; prints `>> line` and the redacted reply).
- `IP=$(cat "$S/badge-ip")`, then from /home/lucas/coding/why2025-badge: `tools/badge.sh ota-info "$IP"`, `tools/badge.sh log "$IP" > "$S/wp-log-N.txt"`, `tools/badge.sh crashes "$IP"`, `tools/badge.sh screenshot "$IP" "$S/wp-N.png"`, `tools/badge.sh ota-file "$IP" <file> <badge-path>`, `tools/badge.sh ota-rm "$IP" <badge-path>`.
- Wrong-token runs: prefix one command with `BADGE_OTA_TOKEN=00000000000000000000000000000000` (well-formed, wrong). Never touch the real token file.
- Before any key: `idle test 0`, then `ui lock on 600` (renew on long steps). When done: `ui open home`, `ui lock off`. One key per call in lists; read `ui` for `focus:` after each; every Enter goes behind a guard `keys send focus:<Row>| enter`. Look at screenshots yourself; describe, do not attach.
- USB console (Part B): the main session only, `tools/badge.sh console` / `monitor` as in the `lichen-bench` skill.

## Allowed commands (the complete list; anything else is not allowed)
Wi-Fi console (agent, Part A): `slot`, `c6`, `crash`, `dev`, `mem`, `mesh`, `onair` (no argument), `help`, `app caps`, `app life`, `app background` and `app background <id> on|off` (step A9 only), `channels`, `mesh loc` (A4 only), `c6log <arg>` (A5 only), `soak`, `boot`, `notify` (count only), `mesh send #badgetest <text>` (A3 only), `factory-reset all x` and `sd format x` (A6 only, wrong word), `token new` (A7 only, expected refusal), `ui`, `ui open <name>`, `ui lock on|off`, `ui overflow`, `ui perf`, `ui answer` (read only; NEVER `ui answer yes`), `idle test 0`, `keys send ...` (Enter only on the rows named in A10), `reboot` (only where a step says so).
`badge.sh`: `ota-info`, `log`, `crashes`, `screenshot`, `ota-file`/`ota-rm` (only on the throwaway paths named in A7, A8, A12), `ota-c6`, `ota-firmware`, `ota-reboot` (main, flash order only).
USB console (main, Part B): `dev`, `dev off`, `dev on`, `channels`, `channel rm '#badgetest'`, `channel add ...`, `channel rm benchwp`, `mesh send #badgetest <text>`, `idle test 0`, `notify test info`, `mesh ident simulate` (dev OFF only, expected refused), `thermal`, `psram`, `sensors`, `stress status`, `token new`, `token` (never pasted), `reboot`.

## Flash order (main session)
- **F0** (before the flash, on the OLD image, owner at the badge, 30 s): Settings > Backup > Save to the card, so a backup in the OLD format (no end line) exists. Serves #453 (C2).
- **F1** C6 bundle FIRST (#452 changes the C6 image): the on-air allow list must be off; `tools/badge.sh ota-c6 "$IP"`; wait for the C6 to come back; `c6` -> `IDENT match`, `up`. Quote the reply. (#447: the C6 push works with the token.)
- **F2** P4 image: `tools/badge.sh ota-firmware "$IP"`; the badge reboots; poll `ota-info` then wait 75 s. (#447: the firmware update works with the token.)
- **F3** `tools/badge.sh ota-info "$IP"` -> `badge_fw fw-<sha>`; save the first-boot log `wp-log-firstboot.txt`. `slot` -> running `(valid)`.
- **F4** Health pass = step A1.

## Part A. Remote (an agent runs it over Wi-Fi; list OFF, developer mode ON, except A12)

**A1. Health (F4) - #452, #455, #446.**
`ota-info`; `slot`; `c6`; `crash`; `dev`; `mem`; `mesh`; `onair`; `app caps`; `help`.
Expected: `badge_fw fw-<sha>`; slot `(valid)`; c6 `IDENT match`, `up`, restarts 0 (both chips on the new images, link up); `crash` this boot clean; `dev` -> developer mode on; `mem` dma free >= 89981 and min >= 80650 (record dma free/min, internal free, psram free; the changes add about 1-4 KB internal .bss in total, quote the deltas against the last wave's figures); `mesh` -> `identity: ok`, record rx, tx, failed, contacts; `onair` -> `on-air allow list off`; `app caps` -> `0 refused since boot`; `help` -> record `N of M commands registered`. Evidence: the lines.

**A2. First-boot log - #446, #452, #233, #449.**
`tools/badge.sh log "$IP" > "$S/wp-log-1.txt"`.
Quote: (a) the line of the developer build's journal self-test at boot (#446: still present in a developer build); (b) no line `journal write of ... failed`; (c) NO notice that `lora.toml` was out of range (#452: a saved config that is valid raises no fallback notice); (d) no line naming a boot-time safe-mode/background mutex problem (#461: boot step ran). Record any `stack margin` line (A11 expects one later).

**A3. Test-channel send and reception - #452, #451, #448, #462.**
`mesh` (note tx T0), `mesh send #badgetest "bench post-soak 1"`, `mesh` after 20 s.
Expected: tx = T0+1, failed unchanged, the peer session reports the message heard on `#badgetest` (the Heltec peer / CoreScope; main asks it; "heard" is the evidence). A real hashtag channel still sends (#451). Then read `log` once more and quote the count of repeater status-poll reply lines (#168 poll) since boot and that no line says a frame was dropped as too long (#448/#462: status replies still arrive; if the poll has not run yet, say so and re-check in A13).

**A4. Position stays out of the log - #449.**
`mesh loc` (save the reply to `$S/wp-loc.txt`, chmod 600, DO NOT print it), then `tools/badge.sh log "$IP" > "$S/wp-log-3.txt"`.
Expected: the asker got the full reply (a line exists in `wp-loc.txt`); in `wp-log-3.txt` the count of lines holding `loc` together with a decimal number such as `12.3456` is 0. Quote the count only. (The Console app tap is the same stream; no screenshot of it.) `set bridge.secret` redaction needs an on-air remote CLI line: NOT run here (covered by `mesh_rset_test`); see the table.

**A5. c6log argument - #458.**
`c6log 10s` -> refused (a malformed-duration reply; quote it). `c6log 0` -> refused. `c6log 10` -> runs (10 ms; quote the first reply line). Then `c6` -> still `up`, restarts unchanged.

**A6. Erase commands refused over Wi-Fi - #455.**
`factory-reset plan all` -> still answers (the plan). `factory-reset all x` -> the USB-only refusal (quote). `factory-reset settings x` -> the same refusal. `sd format x` -> the USB-only refusal (quote). The list is off, so this also proves the refusal holds with the list off. `onair` -> still off. `slot`/`crash` unchanged.

**A7. Token gate on every route, without touching firmware routes - #447, #456, #454, #459.**
For each of `ota-info`, `log`, `crashes`, `screenshot`, a console line (`tools/badge.sh fw "$IP" help`), and `ota-rm "$IP" 'FLASH0:wp-none.bin'` run it once with the WRONG token prefix: each must answer 401 (quote the status line, redacted). Run the same list with the right token: each answers as before (200; `ota-rm` of a missing file answers its normal not-found). A wrong method: `printf 'header = "Authorization: Bearer %s"\n' "$(tr -d '[:space:]' < ~/.config/why2025-badge/ota-token)" | curl --config - -s -o /dev/null -w '%{http_code}\n' -X POST "http://$IP:8080/info"` -> 405 (print the code only). An unknown path -> 404, a malformed head -> 400: pinned by `ota_route_test`; the older, separately filed finding is that these answer before the token check, so do NOT call that a regression. Firmware, C6 and piece routes are NOT probed without a token (a bug there would flash); their refusal is pinned by `ota_route_test`, and F1/F2 showed they work with it. `token new` over Wi-Fi: `"$S/fw.sh" "token new"` -> refused (USB only; quote); then `ota-info` with the right token still works (#456: the token is unchanged).

**A8. File push and delete round trip - #447, #454 (list off).**
Make a 4 KB zero file `$S/wp-test.bin`. `ota-file "$IP" "$S/wp-test.bin" 'FLASH0:wp-test.bin'` -> 200; `ota-rm "$IP" 'FLASH0:wp-test.bin'` -> 200. Leave nothing behind.

**A9. Background list round trip - #461, #233.**
`app background` -> save the list to `wp-bg-before.txt`; pick ONE app id from it that is not MeshCore (MeshCore is always on and has no switch; a refusal for it is expected and fine). `app background <id> on` then `app background` -> the app is removed from the list as the issue describes (quote the one line). Then restore with the inverse form `app background <id> off` and `app background` -> identical to `wp-bg-before.txt` (`diff`). If the inverse does not restore it, STOP and report at once. (The race of two setters is host-tested with threads under TSan; not benched.)

**A10. One UI pass - #444, #233.** (one `ui lock` session; Enter ONLY on the rows named here)
1. Screens that use the shared label helper (#444): `ui open home`, `ui open about`, `ui open timer`, the MeshCore contact list and grid screen (the `ui open` names are listed by `ui open`). One screenshot each; describe: text present, not clipped, status-bar clock ticking (two screenshots 60 s apart on home).
2. Settings > Apps (#233): `ui open settings`, step to `focus: Apps`, `keys send focus:Apps| enter`; one screenshot of the list. For each app row: `keys send focus:<app>| enter`, wait 3 s (the page measures its data), screenshot, `ui overflow`, `esc` back. Expected per page: capabilities note, Data (badge storage and SD card), Background (MeshCore: "Always on"), Crashes, and a Clear data row for every app except MeshCore and the OS apps. NEVER Enter on `Clear data`, on a Background row or on anything inside MeshCore.
3. The Background row against the console: for the app picked in A9, its row reads the same state as `app background` (quote both).
4. `ui open home`, `ui lock off`.

**A11. Overflow, stack margin, memory - #233.**
`ui overflow` -> 0 (after the pass of A10). `tools/badge.sh log "$IP" > "$S/wp-log-11.txt"`; quote every `stack margin` line (the job's 6144 B stack after a measure). `mem` -> compare with A1.

**A12. On-air hold on files - #454. NEEDS THE LIST ON.** Main asks the owner to run the on-air helper `sync` (the list ON makes Wi-Fi updates refused: all flashing is already done) and tells the agent when it is on; after the step the owner runs `off`. Use throwaway names only, never the real `lora.toml`, the MeshCore identity or the token file.
1. `onair` -> list on.
2. `ota-file "$IP" "$S/wp-test.bin" 'FLASH0:lora-wp.tmp'` -> 409 with the hold text (quote). `ota-file ... 'FLASH0:ota_wp.tmp'` -> 409. `ota-rm "$IP" 'FLASH0:[meshcore]wp-none.tmp'` -> 409. A control: `ota-file ... 'FLASH0:wp-test.bin'` -> 200, then `ota-rm` it -> 200 (nothing else is held).
3. Owner runs `off`; `onair` -> list off.
4. With the list off: `ota-file ... 'FLASH0:lora-wp.tmp'` -> 200, `ota-rm` -> 200; same for `FLASH0:ota_wp.tmp`. Leave nothing behind (`ota-rm` both again; a not-found is fine).

**A13. Close - #448, #452, #461.**
`ui` -> stack 0, `ui lock: off`. `slot` `(valid)`; `c6` `IDENT match`, restarts unchanged; `crash` no new crash; `dev` on; `onair` off; `mesh` -> `identity: ok`, tx = T0+1 (only the one test message), failed unchanged; status-poll replies seen by now (#448: quote the count); `mem` against A1 (gate: dma free >= 89981, min >= 80650); `soak` -> quote `soak1` up to `slot=`; `app life` -> summary line and any fault other than `-`.

## Part B. USB console (main session, owner present; Wi-Fi refuses these by design)

**B1. Channel add rules and key entry - #451, #445.**
`channels` -> save the names list (names only) as `before`. Then, with the owner's ok (this removes the test channel for about a minute):
1. `channel rm '#badgetest'`.
2. `channel add '#badgetest' <the well-known Public channel key, as in host_tests/mesh_share_test.c>` -> REFUSED with the "needs that hashtag's key" text, same as a rename (quote). `channels` -> no `#badgetest`.
3. `channel add '#badgetest'` (no key) -> added. `channels` -> the same names as `before`; record whether the order changed.
4. `mesh send #badgetest "bench post-soak 2"` -> tx +1, the peer hears it.
5. Key entry (#445, hex decode): `channel add benchwp 00112233445566778899aabbccddeeff` -> added, `channels` lists it; run the share command `help` names, if any, and quote only the SHAPE of the link (no key); then `channel rm benchwp`. `channels` -> same as `before`.
6. `onair` (the list show, #445) -> the same status as A1.
Marked foreign `#name` in `channels` cannot be created on a clean badge now (the add refuses): host-tested only.

**B2. Developer-mode-off sweep - #421, #455.** (`dev` must read on at the end)
`dev off`. Then each of these must be refused with the developer-mode text (quote one reply, the rest by "same"): `idle test 0`, `notify test info`, `mesh ident simulate` (#455: the test arm is dev-only; if it is NOT refused it blocks LoRa transmit until restart: `reboot`, wait 75 s, and report a FINDING). The plain forms still answer: `thermal`, `psram`, `sensors`, `stress status`, `mesh`. `dev on`; `dev` -> on; `idle test 0` -> accepted.

**B3. Token rotation, USB half - #456. LAST of the remote work, run together with C5.**
Owner first copies the host token file aside as the OLD token (do not print it). `token new` -> a secret line (shown only on the owner's screen; main never pastes it), plus the text that it takes effect at the next restart. `token` -> says the running server keeps the old token until restart. No Wi-Fi call has changed yet: `ota-info` with the old token still works now (quote the status).
Then C5.

**B4. Optional, only with the owner's ok - #385.** After C5 (token settled). On a scratch branch: `tools/badge.sh new-app demo "Demo"`, the full build, OTA the scratch image, `ui open home` screenshot shows the Demo tile; then DROP the branch and re-flash `fw-<sha>` (a second OTA and reboot; this is the one deviation from the single flash, so it is last). Note: lvgl was fetched, so the generator test did not print `SKIP:`.

## Part C. Hands (the owner at the badge, about 9 minutes; the main session reads the console)
Everything here is a physical key press. No remote Enter on any of it.

**C1. Clear data on a test app - #233 (2 min).** Settings > Apps > a small non-OS app (Calc or a similar test app with data) > Clear data. Expected: the careful dialog opens; the app's settings and its two directories are cleared; a restart hint if the app ran; the app's Data row reads 0 afterwards. Main: `ui answer` while it is open -> refused for a remote answer (quote `ui answer: ... not confirmed`), and the dialog stays until the owner answers.

**C2. Settings backup, the old and the new format - #453 (3 min).** (F0 made an old-format backup.) Settings > Backup: Load -> the summary of the old backup shows (the file without an end line still loads); do not apply; cancel. Then Save to the card -> `Saved`; Load -> the same summary as before; cancel. Main afterwards: `log` has no SD write error. Evidence: the two summaries match.

**C3. A chat send from the UI - #451 (1 min).** Messages > `#badgetest` > type one short message and send. Expected: it sends, the peer hears it. Main: `mem` and the task-stack lines right after -> the LVGL stack watermark did not drop below the margin recorded in A11 (quote the figure).

**C4. Repeater login - #462, #288 (1 min, ON AIR to the agreed repeater, ONLY with the owner's word).** One login with the saved password. Expected: login succeeds (a real login reply is 13 bytes and still accepted). If the owner declines, say "not run".

**C5. Token handover - #456 (2 min; after B3).** The owner puts the NEW token in the host token file. Main: `reboot` at USB; wait 75 s. `tools/badge.sh ota-info "$IP"` with the new token works; the same call with the OLD token (`BADGE_OTA_TOKEN=<old>`, set by the owner, not pasted) answers 401; the serial log tool (`tools/serial_log.py`, USB log) shows the secret line hidden (quote "hidden"). `crash` clean. From here the remote part is closed.

## Contradictions and ordering conflicts found between the issues
1. **#452**: the body says a 5 s airtime ceiling, the later decision (2026-11-14) keeps 8 s as built. This script follows the decision; nothing here depends on the number. The bad-preamble `lora.toml` test (push and reboot) is NOT scripted (owner's word needed).
2. **#455 vs earlier scripts**: wave H step E and the #368 proof used a remote SD format; that is refused now. Only the refusal is checked (A6).
3. **#454 vs the standing state**: the hold test needs the list ON, but a list ON refuses Wi-Fi updates and the bench's standing state is off. A12 runs after all flashing and the owner switches it back off.
4. **#456 vs everything remote**: rotating the token cuts the agent's access until the host copy is updated; so it is the last remote action (B3/C5). #455's "USB only" wording also applies to `token new`: A7 only checks its refusal.
5. **#447**: its issue text says the token check is the single gate for every route; its later comment records that 404/405/400 answer before the token check. A7 expects 405/404/400 without the token's help and treats only a 200 without a token as a failure.
6. **#385 vs "one flash"**: the demo-app step needs a second, throwaway image. It is optional and last (B4).
7. **#233 vs the wave-H2 Enter rules**: those forbade Enter on Settings rows. A10 needs Enter on `Apps` and one app row at a time; the owner must allow this (it never reaches `Clear data`, Background or MeshCore).
8. **#461 vs #233**: both touch `app background`; A9 changes the saved list then restores it, and compares with the Settings row (A10.3).

## Table: issue, step(s), what closes it
| Issue | Step(s) | Closes when |
|---|---|---|
| #444 | A10.1 | screenshots of home, clock, About, Timer, MeshCore contacts and grid look as before |
| #446 | A1, A2(a) | boots clean; the dev build still logs the journal self-test |
| #445 | A2(b), B1.5, B1.6 | console key entry, `onair` show and the share link behave as before |
| #447 | F1, F2, A7, A8 | info, log, screenshot, console, file push/delete, firmware update, C6 push all work with the token; the non-flashing routes answer 401 without it; wrong method 405 |
| #385 | B4 (optional) | the Demo tile shows on home after the update; branch dropped, `fw-<sha>` restored |
| #233 | A9, A10.2-3, A11, C1 | a page per app, `ui overflow` 0, a `stack margin` line, Background row equals `app background`, one Clear data works by hand and refuses remote |
| #451 | B1.1-4, C3 | foreign-key add refused, restored channel sends, UI chat send stack margin ok |
| #453 | F0, C2 | old-format backup loads, a new Save then Load shows the same summary |
| #458 | A5 | `c6log 10s` refused, `c6log 10` runs |
| #454 | A12 | list on: `lora`, `ota_` and MeshCore-dir paths get 409; list off: 200; control path 200 |
| #459 | none of its own | covered by A7 and B3/C5 (the token path works after the change) |
| #456 | A7 (token new refused), B3, C5 | remote `token new` refused; USB rotation: new token works, old gets 401, secret hidden in the log |
| #449 | A4 | no position line in the log after `mesh loc`; the `set bridge.secret` check is NOT run (needs an on-air remote line; host-tested) |
| #455 | A6, B2 | `factory-reset`/`sd format` refused over Wi-Fi with the list off; `mesh ident simulate` refused with dev off |
| #452 | F1, A1, A2(c), A3, A13 | both chips on new images, link up, a test-channel send heard, no fallback notice |
| #448 | A3, A13 | no own bench step: closes when status-poll replies still arrive |
| #457 | none of its own | covered by the Wi-Fi join at F2/A1 |
| #461 | A9, A2(d) | `app background <id> on` then the list shows it removed; boot clean |
| #462 | A3, C4 | no own remote step; closes with a successful repeater login (C4) and A3's clean log; without the owner's word it stays open |
