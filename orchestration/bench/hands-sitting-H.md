# Hands list for the owner (about 10 minutes, AFTER the baseline soak ends on 2026-11-16 03:46 UTC)

Everything else of the earlier 40-minute sitting ran remotely on 2026-11-14 or was closed on remote and host-test evidence (owner decision that day; the evidence is on each issue). A power-off restarts the soak's clock, so none of this before the soak has ended.

## Before you start
- Badge on USB, Wi-Fi up, developer mode on, MeshCore running. The on-air allow list is OFF (it stays off on the bench badge until the program ends).

## 1. Power (about 6 minutes)
- [ ] #281 / #292 `fw poweroff` -> **Expected:** `powering off` and the "Powering off..." goodbye screen; the power button starts it again.
- [ ] #391 right after that boot, hold the physical Esc + Backspace within 10 s -> **Expected:** the "Factory reset: settings" question opens; press Esc, **do not confirm**.
- [ ] #281 `fw noscreen`, `fw reboot`, wait 75 s, then `fw poweroff` -> **Expected:** `powering off (no display: saving, then off)`; the badge stays dark, no restart. Power button to start.
- [ ] #281 / #292 headless boot again, `fw poweroff` then `fw reboot` straight after -> **Expected:** log `restart (console reboot) after a power off was asked: powering off instead`; the badge stays off. Power button; check the display is back.
- [ ] END: `fw dev` -> developer mode still on (`fw dev on` at USB if not).

## 2. Hand-only refusal (about 2 minutes)
- [ ] #377 open the "reset settings" confirm dialog by hand, then at the console `fw ui answer yes <wrong label>` and `fw ui answer yes <the real label>` -> **Expected:** both answer `ui answer: <reason>; not confirmed`, the dialog stays open; press Esc. **If either one confirms, settings are erased: watch the screen.**

## 3. Allow list, re-keyed channel (about 3 minutes, optional)
- [ ] #289 switch the list on with the owner helper (`sync`); add a throwaway channel, allow it at USB, remove it, add the same name with another key, send -> **Expected:** refused; then remove the channel and its list entry; switch the list off again with the helper (`off`). (The audit found the related host-side gap: a channel's name is not its identity for the host guard.)

## Only when you have your own reason
- #288 the repeater login with the saved password (on air to the agreed repeater). Left alone on purpose: the audit found that allow-list names are matched by prefix on the badge.
- #426 the real MeshCore factory reset from the companion app: **erases the identity, contacts, channels and prefs.** The safe half (stop and start saves 560 contacts in 40 ms) is proven.

## Not hand-verified, closed anyway (normal use will show a regression)
#400 the physical Messages key starts a stopped MeshCore; #374 seven physical Enters on About; #229 Keyboard test raw key codes; #286 Esc held 3 s ends a stress run; #355 the panel really lights for the test card; #271 the Signal charts and the Air log heading; #273 a physical Enter after 1 s on the Reset order dialog.
