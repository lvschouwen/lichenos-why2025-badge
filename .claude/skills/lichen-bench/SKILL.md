---
name: lichen-bench
description: "Main-session-only bench procedure for the WHY2025 badge: flash, console, OTA. Never for subagents."
---

# Lichen bench (main session only)

Subagents never run any of this; they stop at a green build and a local commit. Before a bench pass: the last merge is followed by `./firmware/build.sh reconfigure build` so the image is labelled `fw-<sha>` of HEAD.

## Flash

- P4: `BADGE_EXTRA_PORTS=/dev/ttyUSB1 tools/badge.sh flash-fw /dev/ttyUSB1` (about 2.5 min, also writes the assets partition). It keeps `/storage` (MeshCore identity, contacts, channels) by default. Never pass `--wipe-storage` without Lucas saying so; it is a first-install step (the guard hook asks).
- C6 image changes need the C6 firmware and its `.md5` pushed together before the P4 image (protocol versions must match). Never leave a firmware.bin and its .md5 mismatched. Over Wi-Fi: `ota-c6` first, wait for the C6 IDENT match, then `ota-firmware`.
- If a permission check blocks flashing, stop and ask Lucas. Do not route around it.

## Bench console

- `badge.sh` runs on the flash host over ssh (host name from `~/.config/why2025-badge/host`). Wait at least 75 s after any reboot before sending console commands; earlier ones are lost while the C6 is flashed.
- Scripted console: `~/badge/console_session.py <port> <wait_s> <read_s> "cmd@delay" ...` on the flash host, run with `~/badge-venv/bin/python`. It opens the port without resetting the badge.
- Commands start with `fw ` (`fw help`). `fw ui` shows the nav stack and focus; `fw keys send <up|down|left|right|enter|esc|tab|bksp|text:...>` drives the real UI. Always run `fw ui` before an `enter`. Lucas may be holding the badge: only press keys when he allows it.
- The P4 logs at INFO: `ESP_LOGI` lines show. Pick the level by #113: E when a feature is lost until someone acts, W for unexpected but handled, I for normal status.
- See the screen with `tools/badge.sh screenshot <ip> <out.png>` (save outside the repo) and look at it. Lucas still checks what a screenshot can't show (flicker, colours on the panel).
- Never press `enter` on Region & radio, TX power, Custom radio or any other setting that changes what the badge transmits unless Lucas asks: a wrong preset puts the badge on a frequency that may not be legal here. Check `fw mesh` shows the Netherlands preset when done.
- Keep bench walks short; Lucas watches them. Reuse "before" screenshots instead of re-walking. One pass: one screenshot per page, long lists only downward in batches (`fw keys send down down down`), no back-and-forth.
- MeshCore on air: never open a DM or compose screen addressed to a real person, and never send to anyone but the agreed test targets (channel `#badgetest`, or a named test DM target). An open compose screen is one stray Enter from a message to a stranger from Lucas's identity.
  `badge.sh fw|console` and the guard hook enforce it for console lines (`tools/onair_guard.sh`, #287): other channels and contacts not in the gitignored `.claude/onair-allow.local` are refused; `advert interval|manual` is refused (Lucas sets it in MeshCore Settings). A refusal is the rule working: never route around it.
  Console floods (#290) go out under the badge's console scope (`fw mesh scope` shows it; the screens keep the default scope). The badge refuses a console send while it is not set; badge.sh reads `fw mesh scope` before each line that can flood (send, dm, remote including `<who> settings`, telemetry, trace, discover, history with a target, and `advert`) and refuses unless the one exact status line names the allow file's `scope <region>` line, and `mesh scope console <x>` passes only for that line. Changing the console scope switches off every status history poll a console line started (docs/architecture.md, Console flood scope).

## Two-chip gate (#234)

Main runs this after every v2 wave merge, on the freshly flashed image (`ota-c6` first when a wave touched `c6/` or `shared/`), and each wave issue lists it. Contract: `docs/architecture.md` "Two-chip contract". Wait 75 s after each reboot.

1. Cold boot (power cycle, not `fw restart`) reaches home; `fw boot` shows the C6 step done and `fw c6` shows `IDENT match`.
2. Wi-Fi joins and `tools/badge.sh ota-info <host>` answers.
3. MeshCore RX: `fw mesh` shows `rx N` counting up between two reads; one TX to `#badgetest` only (see Bench console).
4. `fw c6` and note `restarts`; `fw keys on`; `fw c6 reset`. After the 75 s wait: one physical key press logs a key (`fw keys send` bypasses the TCA8418, so it proves nothing), LoRa `rx N` counts again, Wi-Fi is back (`ota-info`), and `fw c6` reads `up` with `restarts` one higher than before.
5. Brightness changes (this path runs through the C6): `fw bl <display> <keyboard>` (e.g. `fw bl 40 40`), the panel follows; restore the previous level.
6. `fw mem`: DMA free and DMA min not below the pre-MVP baseline (free 99979, min 89611) minus 10 %. Why: that baseline is what today's image measures, so a fixed 150 KB floor would fail the current firmware; the gate catches a regression, not the existing level.
7. BT off after boot (#197): `fw airplane` prints `airplane off | wifi on | lora on | bluetooth off`. It reads the P4's saved switch, not the C6 controller.
8. `fw tasks`: no owner-less Lichen task (IDF, esp_hosted and LVGL tasks are exempt). Informational until #228 books the Lichen tasks: note the result, do not block on it.

Any failure blocks the next wave merge: report it, do not carry on.

## Never

- Touch other serial adapters or the docker services on the flash host. `tools/badge.sh` only talks to the badge; keep it that way.
- Put IPs, host names, SSIDs or tokens in issues or commits.
