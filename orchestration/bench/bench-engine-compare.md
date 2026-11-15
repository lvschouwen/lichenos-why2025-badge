# Bench script: the display engine comparison (#468, #467, #322), after the flash

Plan step 6 of the #407 comment of 2026-11-15. Run by the MAIN session, remote over Wi-Fi, keyboard locked, developer mode ON, after the safety gate (S1-S4), the ten restarts (R1) and Part A of `bench-post-soak.md`. It decides, from numbers: the default engine, whether the PPA engines and the size they cost stay in the image (#468, owner's option 1: they come out again if the PPA loses), a third frame buffer or not (#467), and 62.45 Hz against 60.0 Hz (#322). File prefix for output: `we-`. Every command name, reply text or page name that no issue comment or source file confirmed is tagged **[CONFIRM]** against `fw help all`.

The three engines are the registry key `disp.engine`: `cpu`, `ppa`, `ppa-async`. Developer mode only, refused in release builds, never saved: every boot starts on `cpu`. Do not reboot during this script without saying so (a reboot returns to `cpu`).

## Rules
- Developer mode must be ON (`dev` -> developer mode on). The refusal with it OFF is checked in Part B2 of the main script, not here.
- On air: nothing. No send in this script; the radio is idle.
- The keyboard stays locked (`ui lock on 600`, renewed) except in the owner's hands slice (H below).
- Gated forms go one per call, alone: `set disp.engine <x>`, `psram dma sweep 16384`, `psram dma 15`, `mesh hist scan 100`, `idle test 0`. Never put one in the same shell command block as another line that reaches the badge.
- Never print addresses, names, tokens, coordinates or message text. Raw output to `$S/we-*.txt`; quote only the lines a step asks for.
- Stop at once and fall back to `cpu` (`set disp.engine cpu`) on: a `rotate: ... -> cpu` fallback line other than where a step allows it, a stale or torn strip in a screenshot, `CORRUPT HEAP`, `ui_check: FOUND`, a canary hit, a flipped byte in the PSRAM gate, or a hang (no answer for 30 s; then the owner presses a key or resets, and the engine is `cpu` again after the boot). A PPA that timed out cannot be selected again until the boot (`cpu` always can).
- Known risks (#468, not fixed): a 2D-DMA error looks like a done; a transfer later than 200 ms plus a frame could write an old strip over newer pixels once; the open IDF bug esp-idf #17855 (PPA with PSRAM ECC).
- The old figures to compare against (fw-6b8febf7, CPU engine): redraw 66 ms, flush 2.9 ms per strip, scroll 14.6 fps. The F0b before-values in `wp-before-*.txt` are the same measurement on the old image.

## Tools
- `S=<the session scratchpad>`; console line: `"$S/fw.sh" "<line>"`; `IP=$(cat "$S/badge-ip")`; from /home/lucas/coding/why2025-badge: `tools/badge.sh log "$IP" > "$S/we-log-N.txt"`, `tools/badge.sh screenshot "$IP" "$S/we-N.png"`.
- Scrolling Contacts remotely: lock, `ui open <contacts page>` **[CONFIRM the page name]**, then the batched key form of the remote-UI recipe (a jump plus a batch of `down`/`up`, never key by key) **[CONFIRM the syntax in the `lichen-bench` skill]**: 60 rows down and back is about 10 s. No Enter, ever.
- `ui perf bench 10` on a screen: the screen must be open first. Pages: home, MeshCore Messages, MeshCore Contacts, the Bench app **[CONFIRM page names]**.

## Allowed commands (the complete list)
Console: `dev`, `mem`, `get disp.`, `get disp.engine`, `set disp.engine cpu|ppa|ppa-async` (one per call), `ui`, `ui open <name>`, `ui lock on|off`, `ui overflow`, `ui perf bench 10`, `ui check` **[CONFIRM: the ui_check findings command]**, `idle test 0`, `keys send` (only `up`, `down`, `esc`, `wait`), `psram dma sweep 16384`, `psram dma 15`, `mesh hist scan 100`, `crash`, `c6`, `slot`, a harmless registry write that saves **[CONFIRM: e.g. set a key to its current value; used only in step 6]**.
`badge.sh`: `log`, `screenshot`, `ota-info`.

## 0. Start
1. `dev` -> on; `slot` `(valid)`; `crash` clean; `c6` `IDENT match`, `up`; `get disp.` -> pclk 48000000, refresh 625; `get disp.engine` -> `cpu`.
2. `mem` -> record dma free/min, internal free, psram free (the baseline for the PPA client's heap).
3. `idle test 0` (own call), `ui lock on 600`.

## Per engine: the same block, in the order cpu, ppa, ppa-async
Repeat steps 1-8 for each engine `E`. `cpu` first, so its figures are the control measured in the same run.

**1. Select.** `set disp.engine E` (own call; skip for `cpu`). `get disp.engine` -> `E`. For `ppa` and `ppa-async`: `mem` -> the difference against step 0.2 is the PPA client's heap (record it; the client is only created at the first strip after the switch, so read `mem` again after step 2).
**2. First redraw.** `ui open home`, one screenshot. Re-read `get disp.engine` -> still `E` (#468: the read-back says the engine before the client exists; a failed client creation falls back to `cpu` with an error line: quote it if it appears, and stop that engine).
**3. Scroll Contacts 10 s.** Open Contacts, scroll 10 s. Read `log` and take the SECOND `flip:` line for this engine (the first also pays for creating the client). Expected (#467, #468): `<E> rotate avg N max N us, N strips, 0 fallbacks`, plus the swap wait and copy figures (average and worst) and `0 timeouts, 0 refused`. Record all numbers in the table below. No `swap: no end of frame` line.
**4. Perf bench.** `ui perf bench 10` on home, Messages and Contacts, three runs each, record the median of redraw, flush per strip and scroll fps; then 60 s idle (read `flip:` again: no timeouts); then the Bench app twice.
**5. Screenshot and checks.** One screenshot on a screen with a status bar and a list: orientation and colours right, no stale or torn strips (describe). `ui overflow` -> 0; `ui check` -> 0 findings **[CONFIRM]**.
**6. A settings save during a redraw (ppa and ppa-async only).** While a screen redraws (an `ui perf bench 10` running), run the harmless saving registry write. Expected: no hang; at most one `swap: no end of frame` warning and at most one `rotate: ppa-async -> cpu, no PPA done in time` line. Quote any.
**7. PSRAM/DMA gate (ppa and ppa-async; cpu already passed in S3).** Each in its own call with the engine active: `psram dma sweep 16384` three times, `psram dma 15` seven times, `mesh hist scan 100`. Expected: no flips (`0 bad bytes`), SD legs `direct 0, bounced 0`. Any flip ends the experiment for that engine.
**8. Hands slice H (owner).** Owner scrolls Contacts by hand for 10 s with the engine active and looks at the picture at 10 % backlight (#254): calm / flicker / tearing, colours and position as before. This is step C1 of the main script; the keyboard is unlocked for it (`ui lock off`), then locked again. A screenshot cannot show tearing. Record the owner's words per engine.

After `ppa-async`: `set disp.engine cpu` (own call), `get disp.engine` -> `cpu`.

## Numbers to fill in (one row per engine; the cpu row is the control)
| Engine | rotate avg / max us | swap wait avg / max | copy avg / max | redraw ms home / Messages / Contacts | flush ms per strip | scroll fps | Bench app | PPA client heap | fallbacks | owner's picture |
|---|---|---|---|---|---|---|---|---|---|---|
| cpu | | | | | | | | 0 | 0 | |
| ppa | | | | | | | | | | |
| ppa-async | | | | | | | | | | |
Old image (F0b): redraw 66 ms, flush 2.9 ms per strip, scroll 14.6 fps.

## Close
`set disp.engine cpu`; `ui open home`; `ui lock off`; `ui` -> stack 0. `slot` `(valid)`; `crash` no new crash; `c6` unchanged; `mem` against step 0.2 (the PPA client's heap stays allocated after a switch back: record it). The census of the serial log must show `task_wdt` 0 for this script.

## Decisions from the numbers (the owner; main writes the table to #468, #467 and #322)
1. **Default engine (#468):** the PPA becomes the default only if it beats `cpu` on scroll fps and on full redraw without making the idle update (about 8 ms) worse, with no fallback and no stale strip. Goal: redraw below 50 ms, scroll above 18 fps (a target, not a promise). The PPA invalidates the whole 1 MB back buffer on every strip (driver fact), which may decide it on its own. If the PPA loses: the engines and the +12 365 B / +9 335 B (`libesp_driver_ppa.a`, `libesp_driver_dma.a`) come out again.
2. **A third frame buffer (#467):** only if the swap wait per frame from the `flip:` line is a visible share of the frame time while scrolling Contacts (it costs another 1 036 800 B of PSRAM and roughly doubles the copy that keeps the buffers equal). Otherwise not.
3. **62.45 Hz or 60.0 Hz (#322):** stay at 62.45 Hz if the owner finds the picture calm at 10 % backlight with no new tearing; otherwise front porch 20 -> 31 first (60.0 Hz), 40 MHz as the fallback.
