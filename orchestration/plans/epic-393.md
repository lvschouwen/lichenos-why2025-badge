# Plan: epic #393 UI and apps

Written 2026-11-13 against local main 0746fd86 (#374, #377, #378, #379, #391 merged locally, not pushed or OTA'd). The badge runs fw-5119efd5 (remote bench passes of 2026-11-13 are on that image).

**Bottom line:** 19 of the 21 open issues are code on main, waiting on a bench. Only #386 and #387 need new code. Most of the epic is a proofing job, and the bench tools #377, #378 and #379 move many steps from `needs-lucas` to remote.

## 1. Issue inventory

| # | Short title | Labels of note | Real status (code checked) | What's left |
|---|---|---|---|---|
| 128 | Shell UI bugs (9 items) | score 8, needs-lucas | on main, mostly benched | 18-19 HOT label (thermal test sheds Wi-Fi); 20 held Down at boot: remote via `fw boot keys down` (#379) |
| 155 | Weather app | needs-lucas | on main, remote pass | panel look/flicker (Lucas); SD csv: remote via `fw sd cat` (#378) |
| 108 | Wi-Fi/LoRa/airplane switches | needs-proofing | on main, LoRa 1-6 pass | 7-14 cut the Wi-Fi console: USB console or Lucas |
| 131 | Long headings, repeater values | needs-proofing | on main, 1-5 pass | 6-7: read-only repeater login (Lucas OK'd 2026-11-13, shared with #117) |
| 136 | Wording and units | needs-proofing | on main | 1 (repeater Restart dialog, Esc), 8 (fixed by #141, re-check), 13 power off (hands) |
| 159 | Wi-Fi analyser | needs-lucas | on main | phone comparison, walk test (hands) |
| 160 | Multiple saved networks | needs-lucas | on main (#235 moved storage) | second network, Forget, open-twin test (hands) |
| 161 | LoRa spectrum | needs-lucas | on main, mostly pass | Heltec peak (Heltec peer session can send on #badgetest), 7.5 dB gap verdict (Lucas), Backspace clear |
| 355 | Card wakes backlight | needs-lucas | on main, 1-3a pass | 3b physical key past the lock cap |
| 387 | Name tag face | area:security | **not started** (no owner-info setting, no `nametag_layout.c`) | all |
| 66 | Factory reset | needs-lucas | on main, console path benched | Settings dialog path (`reset settings` is REFUSE in dialog_remote_policy.c: hands); boot combo page: `fw boot keys esc+bksp`, confirm by hand |
| 75 | Lock screen and saver | needs-lucas | on main, remote pass | saver on the panel, Esc+Bksp holds 9-11 (hands) |
| 133 | Overflow detector + sweep | needs-proofing | done: sweep 0 hits on 58 screens (fw-97c9b310) | one sweep on the current image, then close |
| 135 | Shared widgets | needs-proofing | on main | 2 (#144 closed), 5, 6, 12 (repeater), 14 (SD file), 17 (`notify clear`), 19 |
| 138 | OS screens vs standard | needs-proofing | on main | 2 (wrong password), 3 (SD unmount), 5 (#142 closed, re-check) |
| 169 | Settings backup | needs-lucas | on main, Save/no-op Load pass | Load with changes (now ALLOW: `fw ui answer yes load`), wrong-value refusal, file read via `fw sd cat` |
| 189 | Arrange home and MeshCore grid | needs-lucas | on main, both parts; home passed by Lucas | MeshCore grid checklist (fw-d0e03544): fully remote (Reset tile order is ALLOW) |
| 236 | Boot logo "Lichen OS 2" | needs-proofing | on main (1e8e9af9) | cold boot (Lucas), soft restart and About (remote) |
| 351 | LVGL flag deprecations | needs-lucas | merged 29bb9971, sweep 0 hits | one physical key check (secret field, no key log) |
| 386 | Quick-settings panel | — | **not started**; dependencies exist: card_overlay.c, flashlight (#164, attention.c), notify_mute, lock_combo, lichen_sys_net/radio | all; gesture undecided |
| 123 | Batch follow-ups | needs-proofing | nearly done | Console app recall check (remote), tagged CLI on a real repeater; the "shape keys via `keys send`" item probably went with #375 (verify, then close) |

**Totals:** 21 open: 2 not started, 1 done but still open (#133), 18 on main waiting on a bench.

## 2. Dependencies inside the epic
- **Split app_settings.c before #387 and #386.** Both add rows to Settings > Display. app_settings.c is 921 lines (the style limit is ~800). Splitting the Display page out first (prep task P1) means the two never share a file.
- **#387 before #386.** #386 has a "lock" toggle, and the name tag is a lock-screen option. Land the face first so the panel's lock action has one target. Both also touch screen_ids.h and dialog_remote_policy.c.
- **#386 needs #164's flashlight.** The flashlight is on main (attention.c) but not benched, and #164 belongs to epic #394.
- **The #135, #136 and #138 benches share steps.** Run them as one pass. #131 step 6 and #135 step 12 use the same repeater login.

## 3. Cross-epic dependencies
**This epic needs:**
- **#381 bench tooling:** push and OTA the local main first (#374, #377, #378, #379, #391). W0 depends on `fw ui answer`, `fw sd cat` and `fw boot keys`.
  - #391 makes factory_combo refuse injected keys, while #379 says `fw boot keys esc+bksp` should show the #66 page. Confirm on the bench which one wins.
  - #382 (SDL host UI build) would give #386/#387 screenshot tests. That's nice to have, not a blocker.
  - #380 (debug commands behind developer mode): any new `fw nametag`/`fw qs` test command must be registered with `console_register_dev`.
- **#216 platform:**
  - #225 UI service: lockcheck is in count mode and the 1 h soak is still to do. #386 is an overlay over any app, so it uses `lichen_sys_ui_call`/`lichen_ui_with_group`. Run the soak (0 hits) before #386.
  - #235 settings service and secrets class: the storage path for #387's fields.
  - #234 C6 contract: #386's Wi-Fi toggle must call the existing `lichen_sys_net` function only, with no new C6 message.
  - #180 (apps out of lichen_shell): any later part that moves shell files collides with P1.
- **#394 hardware:** #164 flashlight bench before #386. The #128 HOT label check (18-19) is the thermal path; #281 changes when the shell starts the thermal watchdog.
- **#392 MeshCore:** #126 (the chat focus and stack bug) must land before #386's "never takes focus from a compose line" can be tested on Chat. #117 shares the repeater login with #131 and #135.
- **#395 security:** #387 is listed in #395 as well. Its privacy class (never logged, kept out of the backup) is reviewed there. #116 (NVS encryption) would cover the fields later.
- **#262 performance:** #322 (refresh 50 Hz, wt-3) changes the display timing. #386's overlay must keep `fw ui perf` p95 ≤ 33 ms.

**Others need from this epic:**
- #189's `tile_order` and the backup gap: `home_order` is over the 63-character backup cap, which matters for #169.
- #133's sweep script is the UI rule 12 gate for every epic's new screens.

## 4. Decisions for Lucas
1. **#386 gesture** (blocking): a long press on a shape key (which one?), or a free chord. All six shape keys are taken (shortcut.c).
2. **#386 toggles:**
   - Does LoRa off while MeshCore is sending ask first?
   - Does Wi-Fi off from the panel warn that the console goes?
   - Is airplane mode in the panel?
3. **#387 look and privacy:**
   - layout (name size, QR placement)
   - field limits (name 32? handle, pronouns 16?)
   - the default QR content when the QR field is empty (none?)
   - storage: the secrets class (never in a backup) or plain plus a confirm on export (the spec says "unless confirmed")
   - whether the tag replaces the lock screen or sits on top of it with "Locked"
4. **#161:** is 7.5 dB between the channel line and `fw lora` acceptable?
5. **The hands-only bench list in E393-W1:** pick a session. Also: may `notify clear` run (#135 step 17)? May SD unmount run remotely (#138 step 3)?
6. **#155:** the temperature offset to use (it reads ~4 K warm).

**Missing:** no issue tracks the status-bar unread "99+" overlapping the clock (#128 comment). Main should open one.

## 5. Waves

### E393-W0: remote proofing pass (main only, no code)
- **Issues:** #133, #135, #136, #138, #131, #189, #236, #169, #155, #161, #128 (item 20), #66 (boot page only), #123, #351 (sweep part).
- **Prerequisites:** local main pushed and OTA'd (#381 items above). Lucas's ok for keys and tests (given for fw-5119efd5, so reconfirm for the new image).
- **Tasks:**
  - the UI sweep (`tools/ui_overflow_sweep.py --all`)
  - the open remote steps listed in §1
  - #169 Load with one changed value via `fw ui answer yes load`; a hand-edited bad value is refused; `fw sd cat /sd/lichen/settings.txt` shows no `wifi_` lines
  - `fw sd cat` on weather.csv
  - `fw boot keys down` for #128 item 20 (screenshot after the reboot)
  - the #189 MeshCore grid checklist
  - #236 soft restart and About
  - #161 Heltec peak through the Heltec peer session (#badgetest only)
  - #131/#135 repeater read-only login
- **Acceptance:** each issue gets an evidence comment. Issues that passed in full are closed (#133, #189 and #123 are likely). The rest get one consolidated hands checklist.
- **Host tests:** none (no code). **Security:** no secrets in the comments; `fw sd cat` stays on non-denied paths.
- **Files:** none. Agents: none (bench is main-only).
- **Bugs found** become new issues and are fixed in E393-W2 before new features.

### E393-W1: hands session (Lucas, no code)
- **Issues:** #75, #108 (7-14), #128 (18-19), #155 (panel), #159, #160, #66 (Settings dialog and boot combo confirm), #351 (physical key), #355 (3b), #136 (13), #135 (13 power-off timeout), #236 (cold boot), #161 (verdict).
- **Prerequisites:** W0. Can run in parallel with W2 and W3.
- **Acceptance:** each item ticked on its issue, `needs-lucas` removed, closed.
- **Model:** none.

### E393-W2: fixes from W0 (contents unknown)
- **Prerequisites:** W0.
- **Placeholder:** each bug is its own issue with a failing host test where the logic is pure. The "99+" overlap goes here (status_bar.c, Sonnet).
- **Model per bug:** Sonnet, or Opus if it touches the UI service or lock.
- **Conflict files:** host_tests/CMakeLists.txt and tools/standard-baseline.txt. Run in sequence.

### E393-W3: #387 name tag
- **Prerequisites:** P1, decision 3, W0's sweep baseline. #235 is done on main.
- **Tasks:**
  - **P1 (Sonnet, first, alone):** move the Settings > Display page out of app_settings.c into a new `settings_display.c` (lichen_shell). No behaviour change: the sweep and screenshots are identical. Touches app_settings.c, the new file, shell_parts.h, and the lichen_shell CMakeLists.txt.
  - **T1 (Sonnet), pure part:**
    - `nametag_layout.c/.h` (ESP-free): fit and drop rules for long names, emoji or missing glyphs, empty pronouns, QR present or absent.
    - `nametag_fields` validation.
    - host tests `nametag_layout_test.c` and `nametag_backup_test.c` (the fields are absent from the backup export unless confirmed, or always if the secrets class is chosen).
  - **T2 (Sonnet), after T1:**
    - a screen_saver.c style `SAVER_STYLE_NAMETAG` with the Digital drift
    - a lock-screen option (lock_combo.c/screen_saver.c)
    - Settings > Display > Name tag fields (settings_display.c, text_entry)
    - QR through qr_payload.c and qr_screen.c
    - `fw screensaver test nametag` (dev-gated)
    - a factory-reset plan entry if the key is new
- **Acceptance:**
  - nothing shows until the tag is set and switched on
  - nothing is logged (grep the log store)
  - nothing transmits
  - `fw ui overflow` = 0 with a 32-character name
- **Host tests:**
  - layout: long, emoji, empty
  - backup exclusion: fails before T1, passes after
- **Bench:**
  - remote: `fw screensaver test nametag` screenshot, sweep, `fw sd cat` of a backup (no fields)
  - Lucas: the QR scans on a phone; the panel look
- **Security review:**
  - the fields never go to the log or console
  - the backup path
  - the factory reset erases the fields
- **Files:**
  - lichen_shell: screen_saver.c, saver_layout.c/.h, lock_combo.c, settings_display.c, shell_parts.h
  - lichen_api: qr_payload.c, screen_ids.h if a screen is needed
  - lichen_os: settings_secret_keys.c if the secrets class is chosen, factory_plan
- **Conflict files:** host_tests/CMakeLists.txt, the lichen_shell CMakeLists.txt, tools/standard-baseline.txt, and possibly tools/gate_manifest.txt.
- **Parallel:** T1 runs alongside P1 (disjoint files). T2 runs after both.
- **Reviewer:** Sonnet lichen-reviewer for the UI, plus the #395 security reviewer for privacy. Focus: UI rules 1, 2, 8 and 12, burn-in drift, PSRAM placement of the QR buffer, the secret class.

### E393-W4: #386 quick-settings panel
- **Prerequisites:** decisions 1-2, W3 merged (the lock target and settings_display.c), the #225 soak at 0 hits, #164 benched, #126 merged (#392).
- **Tasks:**
  - **T1 (Sonnet):** pure `qs_model.c/.h`. A toggle list with value, action and enabled flag, plus a gate rule (dialog, update gate, setup, boot, test open: closed). Host test `qs_model_test.c` covers the toggles, "a gate blocks opening" and the toggle-to-OS-call mapping.
  - **T2 (Opus):**
    - the overlay on top of card_overlay.c or its own layer through `lichen_sys_ui_call` and `lichen_ui_with_group`
    - focus save and restore (it never steals a compose line's focus)
    - Esc closes it
    - the gesture in shortcut.c/shortcut_decide plus shell.c key routing (the raw-hook table if it's a long press)
    - each toggle calls the same lichen_sys_display/net/radio and notify_mute function Settings uses
    - flashlight_open
    - lock now
    - `fw qs` dev-gated
- **Acceptance:**
  - one state source: Settings shows the change at once
  - hidden under a dialog, the update gate and setup
  - opens over Chat (#126 fixed)
- **Host tests:** `qs_model_test.c` and an extended `shortcut` test for the gesture (fails before, passes after).
- **Bench, remote** (`fw keys press` with hold for a long press):
  - opens over Chat
  - brightness shows in `fw bl`
  - do-not-disturb shows in `fw notify`
  - `fw ui overflow` 0
  - `fw ui lockcheck` 0 hits
  - `fw ui perf` p95 ≤ 33 ms
  - the Wi-Fi toggle is not tested remotely (it cuts the link): Lucas or the USB console
- **Security:** the panel's lock and Wi-Fi off must not be reachable from injected keys if Lucas rules them hand-only (#391 origin API).
- **Performance:** no PSRAM-to-internal-RAM regressions; overlay allocations go through the mem book.
- **Files:** new lichen_shell qs_panel.c and qs_model.c/.h, shortcut.c/.h, shell.c, card_overlay.c, shell_parts.h; lichen_api screen_ids.h.
- **Conflict files:** dialog_remote_policy.c (if the panel uses a dialog), host_tests/CMakeLists.txt, the lichen_shell CMakeLists.txt, tools/standard-baseline.txt, tools/layers-baseline.txt (if it reaches lichen_sys).
- **Parallel:** T1 is pure and starts as soon as the toggle list is decided. T2 waits on T1's header.
- **Reviewer:** Opus (LVGL lock, group, key routing). Focus:
  - lock ordering
  - the group swap and restore on Esc or app exit
  - the shortcut gate
  - LoRa off mid-send
  - UI rules 6, 8 and 10

## 6. Risks, and what to leave out
**Risks:**
- **The bench gate on the local main.** W0 depends on five unbenched tooling merges. If `fw ui answer` misfires on a REFUSE dialog, that is a #381 bug that blocks W0.
- **The #391 vs #379 conflict** on the factory-reset boot page (see §3).
- **app_settings.c churn.** Without P1, #386, #387 and any W2 fix all edit one file of more than 900 lines.
- **#386 is the only UI-service-critical work.** A focus or group bug strands keys; the count-mode lockcheck may not catch it.
- **#387 privacy and burn-in.** A static name at the saver's brightness burns in, so reuse the drift.
- **#108 7-14 and #160** can strand the badge off Wi-Fi with nobody there. Run them only in W1 with Lucas present.

**Leave out of this program:**
- **#73 Bluetooth** (the third radio for airplane mode): its own large epic item.
- **Hiding tiles (#189 "later")**: not scoped.
- **#161's two-badge range test**: there is only one badge.
- **#382 SDL host UI**: worth having for UI regression tests, but it is a large tooling project.
- **New app ideas** beyond #386 and #387, until W0 and W1 drain the bench backlog.
