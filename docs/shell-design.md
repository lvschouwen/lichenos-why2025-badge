# Lichen: the OS shell, and MeshCore as its first app

Design agreed with Lucas on 2026-10-30 (brainstorm with mockups). Mockups: [mockups/](mockups/)
(content fragments for the brainstorm companion; open in a browser, styling is inline). Builds on the lean firmware
(#61, branch `lean-fw`) and MeshCore's companion on the P4 (#65).

## Intent

The firmware gets a name and a shape. **Lichen** is the OS for the WHY2025 badge; **MeshCore is an application on top
of it**, the first of possibly more. It is for people who took a WHY2025 badge home and want to use it as a MeshCore
device, and it is Lucas's own firmware to play with. A fresh flash should lead a person, without a laptop, to a badge
that is on the mesh, and everything set up then must be changeable later on the badge itself.

## Goals

1. **Boot screen** (mockup `boot-screen.html`, option C "hybrid"): from power-on until home.
   - Top: small Lichen logo, name, version, MeshCore version.
   - A segmented bar, one labelled segment per step: storage, display, PSRAM, C6, Wi-Fi, clock, LoRa, mesh.
     Done segments filled, the current one pulsing.
   - The current step in large text, with a percentage where there is one (C6 flash).
   - The last few log lines, dimmed, with real numbers (free storage, PSRAM, C6 image MD5 check). Geeky is wanted.
   - Bottom: node id and uptime.
2. **First-run setup**, once after a fresh flash (mockup `settings.html`, bottom row).
   - Lichen part: a welcome screen with the logo, then Wi-Fi (scanned list, password typed on the badge keyboard,
     **skippable**).
   - MeshCore part: name on the mesh, then region. Asked by the MeshCore app itself (see Open points).
   - Then on to home.
3. **Home screen** (mockup `home.html`): where the badge lands after boot.
   - **Live tiles** (option B) by default: one wide tile per app with a line of what is going on (MeshCore: unread
     count, latest message, nodes and last heard; Settings: Wi-Fi network; About: version, uptime).
   - An **app grid** (option A) as the alternative, switched in Settings › Display › Home layout.
   - A status bar: Lichen logo, clock, Wi-Fi, LoRa.
4. **Apps**: built into the firmware (no loading of foreign binaries: BadgeVMS's lesson). Each app provides:
   - a name, an icon and its home tile/line;
   - its screens;
   - its own page under Settings;
   - a background part (MeshCore keeps listening while you are on home or in another app).
   - First apps: MeshCore, Settings, About Lichen.
5. **Settings** (mockup `settings.html`, top row):
   - Lichen: Wi-Fi, Display (screen and keyboard brightness, home layout), About Lichen.
   - MeshCore's page: name, region & radio (presets plus Custom…, Advanced: TX power and the separate frequency,
     bandwidth, SF, CR), channels (including adding a #hashtag channel), send advert.
6. **About Lichen** (mockup `lichen.html`): logo, a short text on what a lichen is (a fungus and an alga living as one,
   like the P4 that thinks and the C6 that talks to the world, spreading slowly from stone to stone), then version,
   node id, radio, storage, uptime and clock.
7. **Navigation**: keyboard only. Arrows choose, Enter opens, Esc goes back one level until home.

## Brand (decided)

- Name **Lichen**. Checked against firmware, mesh and badge projects: no clash, only small unrelated GitHub projects
  (a Rust licence tool, a cell-library tool).
- Logo **"symbiosis"** (option C in `lichen.html`): a woven mesh of amber nodes (P4) and teal nodes (C6) around a
  centre node. Must work at 16, 32 and about 100 px.
- Colours: background #101820, text #f2f2f2, dim #8090a0, amber #f5b700, teal #3fc1b0 (the existing UI palette plus
  teal).

## Non-goals (this round)

- Typing and sending messages from the badge keyboard. MeshCore can send (adverts work); writing messages is a
  feature of its own, next round.
- A companion-app port (TCP or BLE). Removed on purpose (0d74899): the badge's screens and console are the UI, and no
  unauthenticated port on a production badge.
- Loading apps from storage or the SD card.
- Message history beyond what the badge keeps in RAM (a "we want to chat" device, not an archive).

## Anti-goals

- A first boot that cannot get onto the mesh without a laptop or the serial console.
- Settings that can only be set during first-run, or only from the console.
- Losing Wi-Fi credentials or the MeshCore identity through any settings action (factory reset of MeshCore must keep
  NVS, as it does now).

## Constraints

- ESP-IDF 6.1 on the P4, LVGL 9 via esp_lvgl_port, 720×720 panel shown rotated 90°, keyboard as an LVGL input device
  (TCA8418).
- Internal DMA-capable RAM stays at 150 KB or more free (`fw mem`); large UI state goes in PSRAM. LVGL runs on core 0,
  the mesh task on core 1.
- The boot screen must be up before the slow steps (C6 check and reflash, about 40 s), so the display starts first,
  as today.
- MeshCore code stays unchanged in the submodule; everything badge-specific stays in our shim and target.
- The console (`fw …`) keeps working for everything; screens and console show the same facts. The boot screen's log
  lines come from the same place as the console log.
- The SX1262 module covers 862–928 MHz only: no 433 MHz presets.
- Pure logic (preset tables, setup state, navigation state, text formatting) goes in ESP-free files with host tests,
  as the rest of the firmware.

## Left to the builder

- How the app interface looks in code, and how the background part of an app is scheduled.
- The exact boot step list and how progress is reported from the existing start-up code.
- How a failed boot step is shown. Suggested: the segment turns red with the reason, and boot carries on where it
  can; no Wi-Fi is "skipped", not fatal.
- The icons for MeshCore and Settings, and the logo as LVGL drawing or image.
- Wi-Fi scanning and storage of credentials (today's NVS namespace `badgevms_wifi` is reused by `fw wifi`).

## Open points (to settle in the plan, or with Lucas)

- The region preset list and values must be checked against MeshCore's own app list. The mockup values are
  placeholders. Known: the local Dutch mesh runs 869.618 MHz, 62.5 kHz, SF7, CR 4/5.
- ~~Where MeshCore's first-run questions appear~~ Decided by Lucas (2026-10-30): the first time the MeshCore app is
  opened. Until then MeshCore listens on the default region (Netherlands, local) with its default name.
- The version number: "0.1.0" in the mockups is a placeholder until the first `fw-v*` tag (#63). The `fw-` prefix
  may become `lichen-`.

## Builder check (2026-10-30, against lean-fw @95a9df6)

I checked the design against the code before planning. The findings:

1. **Boot order: the spec says "display starts first, as today", but today it starts after the C6.** `app_main` runs
   storage → console → `c6link_start` (C6 image check/reflash, ESP-Hosted, Wi-Fi) → backlight → display → keyboard
   → LoRa → mesh. For the boot screen to show the C6 step, the display (and LVGL) must move before `c6link_start`.
   The keyboard still starts after the C6, because C6 EN resets the TCA8418. **Risk:** the display backlight is a PWM
   pin on the C6 (GPIO15). While the C6 is held in reset or sits in its ROM bootloader during a reflash, the
   backlight's state is unknown, and the screen may be dark exactly while it shows the slow step. The plan's first
   shell task is a bench check (`fw c6 reflash` with the display already up). If it goes dark, fall back to: the boot
   screen catches up when the backlight returns, and the C6 percentage is shown on the console only.
2. **Wi-Fi credentials are not in `badgevms_wifi`.** `fw wifi` stores `wifi_ssid`/`wifi_pass` in the NVS namespace
   `badge_fw` (`FW_NVS_NAMESPACE`); the BadgeVMS namespace was only migrated from once. Setup and Settings › Wi-Fi
   use `badge_fw`. Scanning goes through ESP-Hosted's `esp_wifi_scan_start` RPC. Changing networks needs a reconnect
   path in `c6link`; today credentials are only read at boot.
3. **MeshCore settings need no MeshCore changes.** `MyMesh::getNodePrefs()` and `savePrefs()` are public. Name, radio
   (freq/bw/sf/cr) and TX power go into `NodePrefs` and are applied with `radio_set_params` / `radio_set_tx_power`
   from target.h, on the mesh task under the mesh lock. Channels use the existing `fw channel add` code, moved into a
   function both the console and the screen call.
4. **First run needs a definition.** "Fresh flash" = no `setup_done` key in `badge_fw`. A badge that already has Wi-Fi
   credentials (any upgraded badge) skips the Wi-Fi step but still sees the welcome screen once. `/storage/meshcore`
   holding an identity does not count as "set up" for MeshCore: its own `name_set` flag decides that.
5. **The clock needs a time zone.** SNTP gives UTC. The status bar and About show local time with a POSIX TZ string
   in NVS. Default `CET-1CEST,M3.5.0,M10.5.0/3` (the badge's home). Lucas decided: settable now, so Settings ›
   Lichen gets a short zone list (name → POSIX TZ table, host-tested).
6. **Region presets: the Dutch mesh is not MeshCore's EU/UK Narrow.** Community sources give EU/UK Narrow as
   869.618 MHz, 62.5 kHz, SF8, CR 4/8, while the local mesh runs SF7, CR 4/5 (the firmware default). The preset list
   gets "Netherlands (local)" as default plus MeshCore's own presets, with values copied from the MeshCore app source
   in the plan task. Out-of-band presets (433 MHz, US 915 on parts the RA-01SH can't do) are left out, see
   Constraints.
7. **Fonts:** Montserrat 14, 24 and 28 are enabled. The boot screen's large step text and the logo name need one
   bigger size (36 or 48), about 30-60 KB of flash each. Cheap enough.
8. **The ordering issues fit as first tasks:** #69 (tick 1000 Hz), then #70 (pinning: moving LVGL to core 1 and the
   mesh task to core 0 also fixes the wrong comment in `mesh_task.cpp`), then #71 step 1 (PPA rotation experiment
   with a software fallback). All three are measured before any shell screen exists, so the numbers aren't
   muddied by new UI.

## Decision: the OS speaks only for itself (Lucas, 2026-10-30)

MeshCore won't be the only app, so Lichen's own screens carry nothing app-specific:
- **Boot screen:** Lichen logo, Lichen version, uptime. The last step is **"apps"** (starting every app's background
  part), with a failing app named in its reason ("MeshCore failed"). No MeshCore version or node id.
- **About Lichen:** OS and hardware only: version, uptime, clock, storage, free memory, and hardware status (C6 link,
  Wi-Fi, LoRa radio present/up). No node id, no radio parameters.
- **Apps describe themselves:** `lichen_app_t` gains an optional about callback; MeshCore's page (its settings) shows
  node id, region/radio parameters and the MeshCore version.
- The LoRa radio stays an OS-level item: it is hardware other apps may use; only how it is tuned belongs to MeshCore.

## Decision: the name is written "Lichen" (Lucas, 2026-10-30)

"Lichen" with a capital L in all text: sentences, titles ("Welcome to Lichen", "Updating Lichen", "About Lichen"), docs,
logs, issues and commits. Only the wordmark next to the logo (boot screen, status bar) is lowercase "lichen", as a
brand style.
