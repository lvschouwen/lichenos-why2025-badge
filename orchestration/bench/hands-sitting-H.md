# Hands sitting H (about 40 minutes left; steps marked DONE ran remotely on 2026-11-14)

## Before you start
- Image: `fw-e93ddf36` (wave H; `fw slot` shows it; do not flash anything).
- Developer mode ON (the bench default), MeshCore running, SD card in, Wi-Fi up, badge on USB.
- The on-air allow list (#289) is OFF on the bench badge until the program ends (owner decision, #420). The #289 and #420 steps in group 4 switch it on with `~/.config/why2025-badge/owner-onair.sh sync` and the group ends with `owner-onair.sh off`.
- A throwaway channel name is needed for the #289 re-key step; a repeater you agreed to log in to (read-only) for #288.
- Time per group: 1 keys at desk 15 min | 2 eyes on the panel 8 min | 3 USB console 12 min | 4 on air 10 min | 5 power 8 min | 6 destructive 3 min.

## 1. Keys at the desk (badge on USB, Wi-Fi up, developer mode on) about 15 min
- [ ] #400 `fw mesh stop`, then press the physical Messages shape key -> **Expected:** MeshCore starts (log has no `starting MeshCore takes a hand` line).
- [ ] #377 open the "reset settings" confirm page/dialog by hand, then `fw ui answer yes <wrong label>`, then `fw ui answer yes <the real label>` -> **Expected:** both answer `ui answer: <reason>; not confirmed` (the reason names the dialog as hand-only or the label as not matching), dialog stays open; press Esc. **If either one confirms, settings are erased: watch the screen.**
- [x] #377 Settings > Reset order dialog (DONE: remote 2026-11-14), `fw ui answer` -> **Expected:** a line starting `ui answer: ALLOW` naming the confirm label; then `fw ui answer no` -> `ui answer: Esc sent`, the dialog closes, the order is kept. (**`answer yes` here resets your arranged home order: only if you accept that.**) Then `fw ui answer no` with no dialog -> `ui answer: no dialog open`.
- [x] #273 Settings > Display > reset home order (DONE: remote 2026-11-14 (cancel and timeout); the confirm is optional): Esc, then let the countdown run out -> **Expected:** log `answered: cancel`, `answered: cancel`; (**Enter after more than 1 s logs `answered: confirm` and resets your arranged home order: only if you accept that**); text "Reset the home order? The tiles go back to their first places."; no `could not be shown (another dialog is opening)`.
- [x] #274 Notes: delete the test note (DONE: remote 2026-11-14) "bench 274 note edited" and confirm with Enter -> **Expected:** "Delete this note?" countdown, then the note is gone ("Deleted.").
- [ ] #274 Files: copy a large /storage file to the SD card; meanwhile on the host `badge.sh ota-rm` that file -> **Expected:** 409 `couldn't delete ...: Permission denied`, file intact, copy completes.
- [ ] #286 `fw stress 1`, then hold Esc 3 s -> **Expected:** "Stress test running" over "keys are locked • hold Esc 3 s to stop", then "Stress test done" and "FAIL: esc".
- [x] #286 Notifications > "Clear all" (DONE: remote 2026-11-14), confirm with Enter (erases your notification list) -> **Expected:** dialog closes, the list refills.
- [ ] #229 Apps > Keyboard test -> **Expected:** raw key codes show, hold-to-exit works; then `fw app caps` -> `0 refused since boot`.
- [ ] #229 Notes: save a note; Timer: let one fire (PARTLY DONE: Notes done remotely; the Timer half stays) -> **Expected:** note saved to the SD card, timer notice and tile badge appear, `fw app caps` still 0 refused.
- [ ] #349 MeshCore: open the #badgetest chat with a physical Enter and close it with Esc, 20 times; `fw mem` before and after -> **Expected:** PSRAM flat (no steady growth).

## 2. Eyes on the panel (about 8 min)
- [ ] #355 let the screen blank, then `fw ui card` -> **Expected:** the panel lights at the saved level at once; with pocket lock on and past the cap, a physical key lights the screen but it stays locked.
- [ ] #335 Settings > Screen slider at 10 -> **Expected:** track is visible on the real panel (note whether the Screen knob sits further left than the Keyboard knob at the same value; the issue asks if that is intended).
- [ ] #264 Crash log: open an entry, Enter to delete, confirm the careful dialog (deletes one crash entry) -> **Expected:** full text shown, countdown runs, entry gone; `fw mem` shows DMA/internal free about 18 KB above the older baseline.
- [ ] #128 steps 18-19 are in group 3 (they need the console).

## 3. USB console (developer mode off, then on again) about 12 min
Run `fw dev off` on the USB console first (needed for the #374 steps; the #421 steps are done).
- [x] #421 `fw dev off` (DONE: remote 2026-11-14, #421 closed) -> **Expected:** `developer mode off`.
- [x] #421 `fw crash test panic` (DONE: remote 2026-11-14) and `fw boot fail clock` -> **Expected:** both answer `<command>: developer mode is off (About: press Enter 7 times, or USB: fw dev on)`, no restart; `fw crash` and `fw boot` still answer.
- [x] #421 `fw psram test 64` (DONE: remote 2026-11-14), `fw thermal test warn`, `fw noscreen`, `fw stress`, `fw idle test 30`, `fw notify test info`, `fw vib test`, `fw setup reset`, `fw sensors stop`, `fw sensors boot off` -> **Expected:** each answers the same refusal and nothing happens (no vibration, no notice, no HOT label).
- [x] #421 `fw psram`, `fw thermal` (DONE: remote 2026-11-14), `fw idle`, `fw notify`, `fw sensors`, `fw stress status` -> **Expected:** each answers its normal status. `fw reboot`, wait 75 s -> the display starts and no welcome screen appears (noscreen and setup reset wrote nothing).
- [ ] #374 About: press Enter 7 times -> **Expected:** toast "Developer mode on" (body "Switch it off in About.") and the amber DEV chip appears.
- [ ] #374 Enter on About -> **Expected:** "Switch developer mode off? The developer rows and commands go away."; confirm with the physical Enter -> developer mode off again.
- [ ] #335 `fw dev on` at USB, then `fw c6 reset`, wait for Wi-Fi back, press one physical key -> **Expected:** the key registers, display and backlight unchanged.
- [ ] #128 `fw thermal test warn` (sheds Wi-Fi; USB stays) -> **Expected:** the HOT label shows in the status bar; `fw thermal test clear` removes it (visual check).
- [ ] END of group: `fw dev status` -> **Expected:** developer mode ON and the DEV chip shown.

## 4. On air (only #badgetest and the test node) about 12 min
- [ ] #271 the test node posts on #badgetest (PARTLY DONE: notice and tile done remotely; chat view after #438) -> **Expected:** a notification, the home tile updates, the message appears in the chat; with the chat open a second post gives no notification; MeshCore > Air log shows heading `Air log`, newest first.
- [ ] #271 switch the test node off, then send it a DM -> **Expected:** a `Not delivered: <text>` notice with the text; no `assert`, no watchdog.
- [ ] #271 test node contact > Signal -> **Expected:** charts "Signal strength" and "Signal to noise" with samples.
- [ ] #289 / #420 switch the list on: `~/.config/why2025-badge/owner-onair.sh sync` -> **Expected:** `on-air allow list on`, 1 of 8 entries. Then in the Console app on the badge: `fw onair off` -> `on-air allow list not changed: switching it off and editing it work only from the USB console`.
- [ ] #289 console DM or trace to a contact that is not on the list -> **Expected:** `on air refused: <target> is not on the console allow list`, nothing transmitted.
- [ ] #289 throwaway channel: add, allow at USB, remove, add the same name with another key, send -> **Expected:** refused; then remove it and its list entry.
- [ ] #420 `owner-onair.sh off`; tell Claude to push an update with no reboot; when Claude says it is installed, run `fw onair on` at USB -> **Expected:** refused because an update is installed but not rebooted (quote the line to Claude); Claude then reboots the badge. The list stays OFF afterwards (`fw onair` -> off).
- [ ] #288 repeater login with the saved password (the agreed repeater, read-only) -> **Expected:** logs in without asking for the password; `fw log` shows no `app secrets:` line at that boot.

## 5. Power (power-off, no-display boot) about 8 min
- [ ] #281 / #292 normal `fw poweroff` -> **Expected:** `powering off` and the "Powering off..." goodbye screen; power button starts it again.
- [ ] #391 right after that boot hold the physical Esc + Backspace within 10 s -> **Expected:** the "Factory reset: settings" question opens; press Esc, **do not confirm**.
- [ ] #335 on the same cold boot -> **Expected:** the badge boots to home with the screen lit (the issue gives no string).
- [ ] #281 `fw noscreen`, `fw reboot`, wait 75 s, then `fw poweroff` -> **Expected:** `powering off (no display: saving, then off)`; the badge stays dark, no restart. Power button to start.
- [ ] #281 / #292 headless boot again, `fw poweroff` then `fw reboot` straight after -> **Expected:** log `restart (console reboot) after a power off was asked: powering off instead`; the badge stays off. Power button; check the display is back.
- [ ] END: `fw dev status` -> developer mode still ON (re-run `fw dev on` at USB if not).

## 6. Last: destructive
- [ ] #426 `fw mesh stop` then `fw mesh start` -> **Expected:** `MeshCore stopped ...: ... saved in N ms`, not `NOT saved`; a reboot from the companion without a reset keeps contacts, channels and prefs.
- [ ] **#426 Factory reset from the companion app (only if you accept losing them or have a backup of contacts, channels and the private key) -> Expected: log `factory reset from the app: MeshCore's files erased, NVS (Wi-Fi) kept`, then `MeshCore reboot`, no save line between; after the restart a new identity, no contacts, only the default channel, default prefs, Wi-Fi still joined. ERASES contacts, channels, prefs and the identity.**

## Left out and why
- #11, #372, #12 (soldering, meter or hardware bodge, as asked); #335 cold boot is the only hardware bit kept.
- Feature wishes with no code or no quick check: #189, #182 (covered by #377), #171, #169, #167, #164, #163, #162, #161, #160, #159, #155, #153, #117, #111, #75, #66, #39, #291.
- #351 (one physical-key logging check: `fw keys on`, Tab in a secret field, press a letter, expect no `shell: key` line; add it to group 1 if time remains).
- #269, #267, #265 (memory moves, proofing is a measurement, not a hand check), #237 (close-out, no hand step).

## Unclear steps or strings that no longer match
- #349 is CLOSED; its leak check was explicitly deferred to this sitting, so it is included.
- #374 and #391 title say "tap Version"; the code counts 7 Enter presses anywhere on About (no Version row). Steps use Enter.
- #274 says Notes shows `Done.`; code shows "Saved." and "Deleted." in the Notes screen (`Done.` is only the Files result). Steps use the code's strings.
- #421: the full sweep is on this image; group 3 tests it with developer mode off. #128 (`thermal test`), #281 (`noscreen`) and #286 (`stress`) need developer mode ON: they sit after `fw dev on` or in groups where it is on.
- #281 and #292: the issue text says the poweroff-then-reboot log line is `restart (console reboot) after a power off was asked: powering off instead`; confirmed in code (console.c).
- #377: a refusal reads `ui answer: <reason>; not confirmed`; a cancel reads `ui answer: Esc sent` (ui_answer.c).
- #335 has no stated expected string for the cold boot and the key after `fw c6 reset`.
- #426 needs the companion app (a phone or desktop client); the ban on phone checks was overridden because the issue was required.
- The "Reset order" confirm (#377, #273) resets your arranged home order: both steps are written so the order is kept unless you choose otherwise. #286 "Clear all" erases your notification list; #264 deletes one crash entry.
