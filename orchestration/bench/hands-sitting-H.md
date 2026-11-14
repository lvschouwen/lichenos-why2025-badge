# Hands sitting H (about 55 to 60 minutes, 20 issues)

## Before you start
- Image label left as `fw-<wave H>` (check `fw about` or the log header; do not flash anything).
- Developer mode ON (the bench default), MeshCore running, SD card in, Wi-Fi up, badge on USB.
- The on-air allow list (#289) is ON with the test channel as its only entry; leave it.
- A throwaway channel name is needed for the #289 re-key step; a repeater you agreed to log in to (read-only) for #288.
- Time per group: 1 keys at desk 15 min | 2 eyes on the panel 8 min | 3 USB console 12 min | 4 on air 10 min | 5 power 8 min | 6 destructive 3 min.

## 1. Keys at the desk (badge on USB, Wi-Fi up, developer mode on) about 15 min
- [ ] #400 `fw mesh stop`, then press the physical Messages shape key -> **Expected:** MeshCore starts (log has no `starting MeshCore takes a hand` line).
- [ ] #377 open the "reset settings" confirm page/dialog by hand, then `fw ui answer yes <wrong label>`, then `fw ui answer yes <the real label>` -> **Expected:** both refused (`dialog '<label>' is hand-only` or a label mismatch), dialog stays open; press Esc. **If either one confirms, settings are erased: watch the screen.**
- [ ] #377 Settings > Reset order dialog, `fw ui answer yes <its label>` -> **Expected:** answered (verdict ALLOW), the order resets (this resets your arranged home order); then `fw ui answer no` with no dialog -> `ui answer: no dialog open`.
- [ ] #273 Settings > Display > reset home order: Esc, then let the countdown run out, then Enter after more than 1 s -> **Expected:** log `answered: cancel`, `answered: cancel`, `answered: confirm`; text "Reset the home order? The tiles go back to their first places."; no `could not be shown (another dialog is opening)`.
- [ ] #274 Notes: delete the test note "bench 274 note edited" and confirm with Enter -> **Expected:** "Delete this note?" countdown, then the note is gone ("Deleted.").
- [ ] #274 Files: copy a large /storage file to the SD card; meanwhile on the host `badge.sh ota-rm` that file -> **Expected:** 409 `couldn't delete ...: Permission denied`, file intact, copy completes.
- [ ] #286 `fw stress 1`, then hold Esc 3 s -> **Expected:** "Stress test running" over "keys are locked • hold Esc 3 s to stop", then "Stress test done" and "FAIL: esc".
- [ ] #286 Notifications > "Clear all", confirm with Enter (erases your notification list) -> **Expected:** dialog closes, the list refills.
- [ ] #229 Apps > Keyboard test -> **Expected:** raw key codes show, hold-to-exit works; then `fw app caps` -> `0 refused since boot`.
- [ ] #229 Notes: save a note; Timer: let one fire -> **Expected:** note saved to the SD card, timer notice and tile badge appear, `fw app caps` still 0 refused.
- [ ] #349 MeshCore: open the #badgetest chat with a physical Enter and close it with Esc, 20 times; `fw mem` before and after -> **Expected:** PSRAM flat (no steady growth).
- [ ] #289 Console app on the badge: `fw onair off` -> **Expected:** `on-air allow list not changed: switching it off and editing it work only from the USB console`.

## 2. Eyes on the panel (about 8 min)
- [ ] #355 let the screen blank, then `fw ui card` -> **Expected:** the panel lights at the saved level at once; with pocket lock on and past the cap, a physical key lights the screen but it stays locked.
- [ ] #335 Settings > Screen slider at 10 -> **Expected:** track is visible on the real panel (note whether the Screen knob sits further left than the Keyboard knob at the same value; the issue asks if that is intended).
- [ ] #264 Crash log: open an entry, Enter to delete, confirm the careful dialog (deletes one crash entry) -> **Expected:** full text shown, countdown runs, entry gone; `fw mem` shows DMA/internal free about 18 KB above the older baseline.
- [ ] #128 steps 18-19 are in group 3 (they need the console).

## 3. USB console (developer mode off, then on again) about 12 min
Run `fw dev off` on the USB console first.
- [ ] #421 `fw dev off` -> **Expected:** `developer mode off`.
- [ ] #421 `fw crash test panic` and `fw boot fail clock` -> **Expected:** both answer `developer mode is off (About: tap Version 7 times, or USB: fw dev on)`, no restart; `fw crash` and `fw boot` still answer.
- [ ] #374 About: press Enter 7 times -> **Expected:** toast "Developer mode on" (body "Switch it off in About.") and the amber DEV chip appears.
- [ ] #374 Enter on About -> **Expected:** "Switch developer mode off? The developer rows and commands go away."; confirm with the physical Enter -> developer mode off again.
- [ ] #335 `fw dev on` at USB, then `fw c6 reset`, wait for Wi-Fi back, press one physical key -> **Expected:** the key registers, display and backlight unchanged.
- [ ] #128 `fw thermal test warn` (sheds Wi-Fi; USB stays) -> **Expected:** the HOT label shows in the status bar; `fw thermal test clear` removes it (visual check).
- [ ] #289 USB: `fw onair` -> **Expected:** on, 1 of 8 entries; optional `fw onair off` to return to an unrestricted console (leave ON for group 4).
- [ ] END of group: `fw dev status` -> **Expected:** developer mode ON and the DEV chip shown.

## 4. On air (only #badgetest and the test node) about 10 min
- [ ] #271 the test node posts on #badgetest -> **Expected:** a notification, the home tile updates, the message appears in the chat; with the chat open a second post gives no notification; MeshCore > Air log shows heading `Air log`, newest first.
- [ ] #271 switch the test node off, then send it a DM -> **Expected:** a `Not delivered: <text>` notice with the text; no `assert`, no watchdog.
- [ ] #271 test node contact > Signal -> **Expected:** charts "Signal strength" and "Signal to noise" with samples.
- [ ] #289 console DM or trace to a contact that is not on the list -> **Expected:** `on air refused: <target> is not on the console allow list`, nothing transmitted.
- [ ] #289 throwaway channel: add, allow at USB, remove, add the same name with another key, send -> **Expected:** refused; then remove it and its list entry.
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
- #421 owner decision (gate all other test commands: thermal test, psram, noscreen, stress, idle test, notify test, vib test, setup reset, sensors) is NOT on main yet (they are still plain `console_register`); group 3 therefore tests only `crash test` and `boot fail`, and #128 and #281 still use `thermal test` and `noscreen` with developer mode on.
- #281 and #292: the issue text says the poweroff-then-reboot log line is `restart (console reboot) after a power off was asked: powering off instead`; only the tail "powering off instead" was found in code (app_power.c, console.c), the full wording is unconfirmed.
- #377 "dialog '<label>' is hand-only" is in code (ui_answer.c); the "wrong label" refusal wording was not checked.
- #335 has no stated expected string for the cold boot and the key after `fw c6 reset`.
- #426 needs the companion app (a phone or desktop client); the ban on phone checks was overridden because the issue was required.
- #264 and #286: "Reset order" confirm resets your arranged home order; #286 "Clear all" erases your notification list.
